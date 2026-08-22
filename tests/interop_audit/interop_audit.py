#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX interop audit: the shipped protocol clients against real upstreams
#
# Not adversarial like the other suites: nothing here sends malformed bytes on purpose. This
# proves WFX::PostgresEndpoint, WFX::SmtpEndpoint and WFX::HttpEndpoint actually interoperate
# with real, spec-compliant servers, every leg on real TLS, the way a real deployment would run
# them. Postgres and SMTP are real Docker containers (a second SMTP instance forces the AUTH
# LOGIN fallback path); the HTTP upstream is a second, ordinary WFX server running real HTTPS,
# not a third-party image, so that leg proves WFX's own client and server genuinely interoperate.
#
# `python3 interop_audit.py` alone brings up Docker (generating and permission-fixing its own
# TLS cert first), boots the HTTPS upstream, then drives the app under test. See README.md.
#
# Usage:
#   python3 interop_audit.py                   # all phases
#   python3 interop_audit.py --phase smtp
#   python3 interop_audit.py --no-docker        # you already ran `docker compose up -d`
#   python3 interop_audit.py --keep-docker      # leave containers running after this run
#
# Exit codes:
#   0   all phases passed
#   1   a correctness failure, or the worker died during the run
#   3   WFX never answered /health, so nothing was exercised

import copy
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.request

# Suites are run directly, so tests/ has to be on the path before common is importable
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import common
from common import net, term
from common.server import Server, tls_probe

HERE = os.path.dirname(os.path.abspath(__file__))
CERTS_DIR = os.path.join(HERE, "certs")
COMPOSE_FILE = os.path.join(HERE, "docker-compose.yml")

# EndpointStatus, mirrors shared/abis/types.hpp, keep in sync
EP_SUCCESS = 0

UPSTREAM_PORT = 8443
SMTP_API_PLAIN = 5080
SMTP_API_LOGIN = 5081
SMTP_PORT_PLAIN = 2525
SMTP_PORT_LOGIN = 2526
PG_PORT = 5533

# Certs
def openssl(args):
    return subprocess.run(["openssl"] + args, capture_output=True, text=True)

def ensure_certs():
    """One self-signed cert (SAN 127.0.0.1/localhost), reused by Postgres, both smtp4dev
    instances and the HTTPS upstream. Idempotent, skipped once already generated.

    Postgres additionally gets its own copy, chowned to the postgres user's uid *inside the
    postgres:16-alpine image* (looked up rather than hardcoded, in case it ever changes) and
    chmod 600, since it refuses to start otherwise. The chown needs root, which a throwaway
    container gives us without needing sudo on the host.
    """
    shared_dir = os.path.join(CERTS_DIR, "shared")
    pg_dir = os.path.join(CERTS_DIR, "pg")

    if os.path.exists(os.path.join(shared_dir, "server.crt")):
        return

    os.makedirs(shared_dir, exist_ok=True)
    os.makedirs(pg_dir, exist_ok=True)

    made = openssl(["req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", os.path.join(shared_dir, "server.key"),
                    "-out", os.path.join(shared_dir, "server.crt"),
                    "-days", "365", "-subj", "/CN=localhost",
                    "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"])
    if made.returncode != 0:
        raise RuntimeError("could not generate interop_audit's TLS cert, is openssl broken?\n%s" % made.stderr)

    term.log("certs", term.green("generated shared/server.{crt,key}"))

    shutil.copyfile(os.path.join(shared_dir, "server.crt"), os.path.join(pg_dir, "server.crt"))
    shutil.copyfile(os.path.join(shared_dir, "server.key"), os.path.join(pg_dir, "server.key"))
    os.chmod(os.path.join(pg_dir, "server.key"), 0o600)

    uid_probe = subprocess.run(["docker", "run", "--rm", "postgres:16-alpine", "id", "-u", "postgres"],
                               capture_output=True, text=True)
    uid = uid_probe.stdout.strip()
    if uid_probe.returncode != 0 or not uid.isdigit():
        raise RuntimeError("could not look up postgres:16-alpine's postgres uid:\n%s" % uid_probe.stderr)

    chown = subprocess.run(["docker", "run", "--rm", "-v", "%s:/certs" % pg_dir, "alpine:3.20",
                            "chown", "%s:%s" % (uid, uid), "/certs/server.key"],
                           capture_output=True, text=True)
    if chown.returncode != 0:
        raise RuntimeError("could not chown the Postgres TLS key inside a container:\n%s" % chown.stderr)

    term.log("certs", term.green("pg/server.key owned by uid %s, mode 600" % uid))

# Docker Compose
def docker_compose(*args):
    return subprocess.run(["docker", "compose", "-f", COMPOSE_FILE, *args], cwd=HERE,
                          capture_output=True, text=True)

def wait_tcp(host, port, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            socket.create_connection((host, port), timeout=1.5).close()
            return True
        except OSError:
            time.sleep(0.3)
    return False

def ensure_docker_up(cfg):
    if shutil.which("docker") is None:
        raise RuntimeError("docker not found on PATH, interop_audit needs it for Postgres and SMTP, see README.md")

    ensure_certs()

    term.log("docker", "bringing up postgres + smtp4dev x2 ...")
    up = docker_compose("up", "-d", "--wait")
    if up.returncode != 0:
        raise RuntimeError("docker compose up failed:\n%s" % (up.stderr or up.stdout))

    for label, port in (("postgres", PG_PORT), ("smtp4dev", SMTP_PORT_PLAIN), ("smtp4dev-loginonly", SMTP_PORT_LOGIN)):
        if not wait_tcp(cfg.host, port, 30):
            raise RuntimeError("%s never accepted a connection on :%d" % (label, port))

    term.log("docker", term.green("up: postgres :%d, smtp4dev :%d/:%d" % (PG_PORT, SMTP_PORT_PLAIN, SMTP_PORT_LOGIN)))

# Config patching (cert_path/key_path/outbound_ca_path are "..." placeholders in git, the
# harness fills in the real absolute paths, same pattern client_audit uses for its own certs)
def patch_toml_value(toml_path, key, value):
    import re
    with open(toml_path) as f:
        text = f.read()

    pattern = r'(?m)^(\s*%s\s*=\s*)"[^"]*"' % re.escape(key)
    text, n = re.subn(pattern, lambda m: m.group(1) + '"%s"' % value, text)
    if n == 0:
        raise RuntimeError("%s: no '%s = \"...\"' line to patch" % (toml_path, key))

    with open(toml_path, "w") as f:
        f.write(text)

def patch_configs():
    shared_crt = os.path.join(CERTS_DIR, "shared", "server.crt")
    shared_key = os.path.join(CERTS_DIR, "shared", "server.key")

    upstream_toml = os.path.join(HERE, "upstream", "config", "wfx.local.toml")
    patch_toml_value(upstream_toml, "cert_path", shared_crt)
    patch_toml_value(upstream_toml, "key_path", shared_key)

    app_toml = os.path.join(HERE, "app", "config", "wfx.local.toml")
    patch_toml_value(app_toml, "outbound_ca_path", shared_crt)

    term.log("patch", "outbound TLS trust + HTTPS upstream cert -> %s" % shared_crt)

# The HTTP upstream: a second, ordinary WFX server, on real HTTPS
def start_upstream(cfg):
    ucfg = copy.copy(cfg)
    ucfg.port = UPSTREAM_PORT
    ucfg.app_dir = os.path.join(HERE, "upstream")

    server = Server(ucfg, flags=("--use-https", "--https-port-override"), cwd=HERE, app="upstream",
                    probe=tls_probe, label="/health (HTTPS upstream)")
    server.start()
    if server.wait_ready() is None:
        raise RuntimeError("HTTPS upstream never answered /health within %ds" % cfg.ready_timeout)

    return server

# smtp4dev's own REST API: how the harness confirms real delivery, not just what the client
# itself claims. See https://github.com/rnwood/smtp4dev/blob/master/docs/API.md
def smtp4dev_get(port, path, timeout=5.0):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=timeout) as resp:
            return json.loads(resp.read())
    except Exception:
        return None

def smtp4dev_messages(port):
    data = smtp4dev_get(port, "/api/messages?page=1&pageSize=50&sortColumn=receivedDate&sortIsDescending=true")
    if data is None:
        return []
    if isinstance(data, list):
        return data
    for v in data.values():
        if isinstance(v, list):
            return v
    return []

def smtp4dev_message(port, msg_id):
    """Full Message object (id/from/to/subject/secureConnection/...), unlike the lighter
    MessageSummary the list endpoint returns."""
    return smtp4dev_get(port, "/api/messages/%s" % msg_id)

def smtp4dev_plaintext(port, msg_id, timeout=5.0):
    try:
        req = urllib.request.Request("http://127.0.0.1:%d/api/messages/%s/plaintext" % (port, msg_id))
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8", "replace")
    except Exception:
        return ""

def smtp4dev_find(port, subject, timeout=3.0):
    """Polls the message list for one with this subject. smtp4dev acks the client's DATA
    command slightly before the message is indexed and queryable, so a single snapshot right
    after sending can race it."""
    deadline = time.time() + timeout
    while True:
        msgs = smtp4dev_messages(port)
        summary = next((m for m in msgs if m.get("subject") == subject), None)
        if summary or time.time() >= deadline:
            return summary, msgs
        time.sleep(0.2)

def smtp4dev_clear(port, timeout=5.0):
    try:
        req = urllib.request.Request("http://127.0.0.1:%d/api/messages/*" % port, method="DELETE")
        urllib.request.urlopen(req, timeout=timeout)
    except Exception:
        pass

# Driving the app under test
def _json(cfg, method, path, headers=None, body=b"", rtimeout=20.0):
    return net.get_json(cfg.host, cfg.port, method, path, headers, body, rtimeout=rtimeout)

def is_ok(r):
    return bool(r) and r.get("ep") == EP_SUCCESS

# PHASE: postgres
def phase_postgres(ctx):
    cfg = ctx.cfg
    p = ctx.phase("postgres")

    r = _json(cfg, "GET", "/pg/select-one")
    p.check("select-one: connects, SCRAM auth, real TLS, query", is_ok(r), "got %r" % r)
    p.check("select-one: int decodes", is_ok(r) and r.get("one") == 1, "got %r" % r)
    p.check("select-one: bool decodes", is_ok(r) and r.get("flag") is True, "got %r" % r)
    p.check("select-one: text decodes", is_ok(r) and r.get("txt") == "hi", "got %r" % r)

    marker = "roundtrip-%d" % time.time_ns()
    r = _json(cfg, "POST", "/pg/roundtrip", body=marker.encode())
    p.check("roundtrip: real INSERT ... RETURNING", is_ok(r) and r.get("val") == marker, "got %r" % r)
    p.check("roundtrip: id assigned", is_ok(r) and isinstance(r.get("id"), int) and r.get("id") > 0, "got %r" % r)

    # /pg/tx and /pg/savepoint never write a top-level "ep" (there's no single outbound call
    # to report one for), so these check their own *_ep fields directly instead of is_ok(r)

    marker = "tx-commit-%d" % time.time_ns()
    r = _json(cfg, "POST", "/pg/tx", {"X-Finish": "commit"}, marker.encode())
    p.check("tx commit: begin/insert/commit", r and r.get("finish_ep") == EP_SUCCESS, "got %r" % r)
    p.check("tx commit: row visible from a second connection", r and r.get("visible_count") == 1, "got %r" % r)

    marker = "tx-rollback-%d" % time.time_ns()
    r = _json(cfg, "POST", "/pg/tx", {"X-Finish": "rollback"}, marker.encode())
    p.check("tx rollback: begin/insert/rollback", r and r.get("finish_ep") == EP_SUCCESS, "got %r" % r)
    p.check("tx rollback: row NOT visible afterward, real atomicity", r and r.get("visible_count") == 0,
           "got %r" % r)

    for iso in ("read_committed", "repeatable_read", "serializable"):
        marker = "tx-%s-%d" % (iso, time.time_ns())
        r = _json(cfg, "POST", "/pg/tx", {"X-Isolation": iso}, marker.encode())
        p.check("isolation level accepted: %s" % iso, r and r.get("begin_ep") == EP_SUCCESS, "got %r" % r)

    marker = "sp-%d" % time.time_ns()
    r = _json(cfg, "POST", "/pg/savepoint", body=marker.encode())
    p.check("savepoint: begin/savepoint/rollback-to/commit",
           r and all(r.get(k) == EP_SUCCESS for k in ("savepoint_ep", "rollback_to_ep", "commit_ep")),
           "got %r" % r)
    p.check("savepoint: only the pre-savepoint row survived",
           r and r.get("rows") == 1 and r.get("first_val") == marker + "-a", "got %r" % r)

    r = _json(cfg, "POST", "/pg/stream", {"X-Rows": "500", "X-ChunkRows": "37"})
    p.check("stream: all 500 rows arrive", is_ok(r) and r.get("rows_seen") == 500, "got %r" % r)
    p.check("stream: delivered across more than one chunk", r and r.get("chunks", 0) > 1, "got %r" % r)
    p.check("stream: sum matches 1..500", r and r.get("sum") == 500 * 501 // 2, "got %r" % r)

    cache_ok = True
    for _ in range(5):
        rr = _json(cfg, "GET", "/pg/select-one")
        cache_ok = cache_ok and is_ok(rr) and rr.get("one") == 1
    p.check("statement cache: repeated execution stays correct", cache_ok)

# PHASE: smtp
def phase_smtp(ctx):
    cfg = ctx.cfg
    p = ctx.phase("smtp")

    for label, ep_name, api_port in (("AUTH PLAIN", "plain", SMTP_API_PLAIN), ("AUTH LOGIN", "login", SMTP_API_LOGIN)):
        smtp4dev_clear(api_port)

        subject = "interop-%s-%d" % (ep_name, time.time_ns())
        body = "hello from wfx\r\n.\r\nline right after a lone dot\r\n"  # RFC 5321 dot-stuffing

        r = _json(cfg, "POST", "/smtp/send",
                 {"X-Endpoint": ep_name, "X-Subject": subject, "X-To2": "cc@wfx-interop.test"}, body.encode())
        p.check("%s: full transaction completes" % label, is_ok(r) and r.get("success") is True, "got %r" % r)

        summary, msgs = smtp4dev_find(api_port, subject)
        p.check("%s: message actually delivered" % label, summary is not None,
               "no message with subject %r among %d" % (subject, len(msgs)))

        if summary:
            detail = smtp4dev_message(api_port, summary.get("id")) or {}
            p.check("%s: delivered over STARTTLS" % label, detail.get("secureConnection") is True,
                   "got %r" % detail)
            # No To:/Cc: header names the second RCPT TO address, so smtp4dev correctly files
            # it as bcc, real envelope-vs-header semantics, not a to/cc entry
            recipients = (detail.get("to") or []) + (detail.get("cc") or []) + (detail.get("bcc") or [])
            p.check("%s: second envelope recipient (multi-RCPT) delivered" % label,
                   "cc@wfx-interop.test" in recipients, "got %r" % detail)

            text = smtp4dev_plaintext(api_port, summary.get("id"))
            p.check("%s: dot-stuffed body round-trips intact" % label, "line right after a lone dot" in text,
                   "got %r" % text)

    smtp4dev_clear(SMTP_API_PLAIN)
    subject = "interop-sendmail-%d" % time.time_ns()
    r = _json(cfg, "POST", "/smtp/send-mail", {"X-Subject": subject}, b"via SendMail")
    p.check("SendMail(): single-call wrapper completes", is_ok(r) and r.get("success") is True, "got %r" % r)
    summary, msgs = smtp4dev_find(SMTP_API_PLAIN, subject)
    p.check("SendMail(): message delivered", summary is not None,
           "no message with subject %r among %d" % (subject, len(msgs)))

    smtp4dev_clear(SMTP_API_PLAIN)
    r = _json(cfg, "POST", "/smtp/reset")
    p.check("RSET: fresh transaction on the reused connection completes",
           is_ok(r) and r.get("success") is True, "got %r" % r)
    summary, msgs = smtp4dev_find(SMTP_API_PLAIN, "interop-reset")
    p.check("RSET: only the post-reset message was delivered", summary is not None,
           "no message with subject 'interop-reset' among %d" % len(msgs))

# PHASE: http
def phase_http(ctx):
    cfg = ctx.cfg
    p = ctx.phase("http")

    r = _json(cfg, "GET", "/http/call")
    p.check("GET: real HTTPS round trip to the WFX upstream", is_ok(r) and r.get("status") == 200, "got %r" % r)

    for method in ("POST", "PUT", "PATCH", "DELETE"):
        headers = {"X-Method": method, "X-Path": "/echo"}
        if method != "DELETE":
            headers["X-Body"] = "body-%s" % method
        r = _json(cfg, "GET", "/http/call", headers)
        p.check("%s: real upstream round trip" % method, is_ok(r) and r.get("status") == 200, "got %r" % r)
        if method != "DELETE":
            p.check("%s: body round-trips" % method, r.get("body") == headers["X-Body"], "got %r" % r)

    for code in (200, 201, 301, 404, 500):
        r = _json(cfg, "GET", "/http/call", {"X-Path": "/status/%d" % code})
        p.check("status %d passthrough from a real server" % code, is_ok(r) and r.get("status") == code,
               "got %r" % r)

    r = _json(cfg, "GET", "/http/call", {"X-Path": "/basic-auth", "X-Auth": "1"})
    p.check("basic auth: correct credentials accepted", is_ok(r) and r.get("status") == 200, "got %r" % r)
    r = _json(cfg, "GET", "/http/call", {"X-Path": "/basic-auth"})
    p.check("basic auth: missing credentials refused", is_ok(r) and r.get("status") == 401, "got %r" % r)

    r = _json(cfg, "GET", "/http/call", {"X-Path": "/stream/20"}, rtimeout=10.0)
    p.check("chunked stream: all 20 real chunks arrive",
           is_ok(r) and (r.get("body") or "").count("line ") == 20, "got %r" % r)

    # /delay/20000 vastly exceeds Web's requestTimeoutSeconds (15s in main.cpp), against a
    # genuinely slow real server, not a hostile hang
    r = _json(cfg, "GET", "/http/call", {"X-Path": "/delay/20000"}, rtimeout=25.0)
    p.check("client request timeout fires against a real slow server",
           r is not None and r.get("ep") != EP_SUCCESS, "got %r" % r)

class InteropAudit(common.Suite):
    name = "interop_audit"
    description = "The shipped protocol clients against real upstreams, every leg on real TLS"
    phases = {"postgres": phase_postgres, "smtp": phase_smtp, "http": phase_http}
    phase_timeout = 120

    def add_arguments(self, parser):
        parser.add_argument("--no-docker", action="store_true",
                            help="skip docker compose up, assume it's already running")
        parser.add_argument("--keep-docker", action="store_true",
                            help="leave the containers running after the run instead of tearing them down")

    def setup(self, ctx):
        patch_configs()
        if not ctx.cfg.args.no_docker:
            ensure_docker_up(ctx.cfg)
        ctx.resources["upstream"] = start_upstream(ctx.cfg)

    def teardown(self, ctx):
        upstream = ctx.resources.get("upstream")
        if upstream:
            upstream.stop()

        # Torn down by default so a run doesn't leave containers eating resources in the
        # background. --no-docker means the caller is managing them themselves, leave those
        # alone; --keep-docker is the explicit opt-in for someone iterating on this suite
        # who doesn't want to pay image-pull/initdb cost again on the next run.
        if not ctx.cfg.args.no_docker and not ctx.cfg.args.keep_docker:
            term.log("docker", "docker compose down -v ...")
            down = docker_compose("down", "-v")
            if down.returncode == 0:
                term.log("docker", term.green("postgres + smtp4dev x2 removed"))
            else:
                term.log("docker", term.yellow("docker compose down exited %d: %s"
                                               % (down.returncode, (down.stderr or down.stdout).strip())))

if __name__ == "__main__":
    common.run(InteropAudit)
