#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX client audit: the shipped protocol clients
#
# Three clients, one hostile mock each. WFX::HttpEndpoint
# (include/wfx/endpoint/http.hpp) is driven against http_upstream.py, which replays
# exactly the bytes this suite hands it, so every branch and boundary of the
# client-side HTTP/1.1 parser and serializer is reachable. WFX::SmtpEndpoint
# (include/wfx/endpoint/smtp.hpp) is driven against smtp_upstream.py, one listener
# per persona, each running the real EHLO, STARTTLS and AUTH handshake and the
# MAIL, RCPT and DATA transaction behind it, with exactly one thing turned hostile.
# WFX::PostgresEndpoint (include/wfx/endpoint/postgres/*.hpp) is driven against
# postgres_upstream.py the same way: one listener per persona running the real
# SSLRequest, StartupMessage and SCRAM-SHA-256 handshake, with either the handshake
# or one query's response turned hostile.
#
# The audit never talks to a mock's data port: it drives WFX, and WFX drives the
# mock. See README.md for what each phase proves.
#
# Usage:
#   python3 client_audit.py                    # all phases
#   python3 client_audit.py --phase http_desync
#   python3 client_audit.py --list-phases
#
# Exit codes:
#   0   all phases passed
#   1   correctness failure, or the worker died during the run
#   2   security finding: a pooled connection poisoned or desynced, a smuggled
#       request or response body delivered as real, a CR/LF/NUL reaching the wire
#       through a path, a header, an SMTP field or a Postgres auth exchange, one
#       request's headers or body surfacing in another's, coalesced waiters getting
#       the wrong key's response, an SMTP session continuing in plaintext or
#       trusting pre-TLS bytes, or a Postgres auth downgrade or forged SCRAM signature
#       going unnoticed
#   3   WFX never answered /health, so nothing was exercised

import itertools
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

# Suites are run directly, so tests/ has to be on the path before common is importable
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import common
from common import net, term

HERE = os.path.dirname(os.path.abspath(__file__))

# Constants

# EndpointStatus, mirrors shared/abis/types.hpp, keep in sync
EP_SUCCESS           = 0
EP_POOL_EXHAUSTED    = 6
EP_INTERNAL          = 10  # where a co_return EpFatal from SmtpOnConnect lands for every
                           # non-timeout handshake refusal, bad cert or wrong credentials
                           # alike: the coroutine only ever answers EpReady or EpFatal, so
                           # the engine's generic connect-failure funnel classifies it
EP_SERIALIZE         = 11
EP_HANDSHAKE_TIMEOUT = 13
EP_REQ_TIMEOUT       = 14

# EpDefault's connLimit in app/src/main.cpp, compiled in rather than passed on the
# command line. HttpEndpointConfig allocates slots exactly, so this is the real
# ceiling on how many requests can be in flight to that endpoint at once
DEFAULT_CONN_LIMIT = 4

# Staged bytes are keyed by a unique id, so a replay can never pick up a previous one
_stage_ids = itertools.count(1)

# One persona per fixed port: (name, port, cert basename, mock options, expectation).
# Every name and port here MUST match a WFX::SmtpEndpoint declaration in
# app/src/main.cpp, since those are compiled in rather than passed on the command line
SMTP_CONTROL_PORT = 8199

SMTP_PERSONAS = [
    # Happy paths
    ("good",                8100, "good",       {},                                       "accept"),
    ("auth_login_only",     8101, "good",       {"auth_mechanisms": "LOGIN"},             "accept"),
    # CVE-2011-0411 / CVE-2026-41319 class: plaintext spliced in right after the STARTTLS
    # go-ahead. A correct client discards whatever is already buffered before the TLS wrap,
    # so anything still unread on the wire corrupts the handshake's own read and fails
    # closed. This is a refusal, not a transaction that goes through
    ("inject",              8103, "good",       {"inject": "MAIL FROM:<mitm@evil>\r\n"},  "refuse"),
    # Handshake-phase refusals: no STARTTLS offered, or a hostile protocol, cert or AUTH
    ("no_starttls",         8102, "good",       {"starttls": "0"},                        "refuse"),
    ("selfsigned",          8104, "selfsigned", {},                                       "refuse"),
    ("wronghost",           8105, "wronghost",  {},                                       "refuse"),
    ("expired",             8106, "expired",    {},                                       "refuse"),
    ("auth_fail",           8107, "good",       {"auth_fail": "1"},                       "refuse"),
    ("no_auth_mechs",       8108, "good",       {"auth_mechanisms": ""},                  "refuse"),
    ("mismatched_code",     8109, "good",       {"mismatched_code": "1"},                 "refuse"),
    ("malformed_greeting",  8116, "good",       {"malformed_greeting": "1"},              "refuse"),
    ("drop_greeting",       8117, "good",       {"drop_after": "greeting"},               "refuse"),
    ("drop_pre_handshake",  8118, "good",       {"drop_after": "starttls_pre_handshake"}, "refuse"),
    ("drop_starttls",       8119, "good",       {"drop_after": "starttls"},               "refuse"),
    ("drop_auth",           8120, "good",       {"drop_after": "auth"},                   "refuse"),
    ("drop_data_prompt",    8121, "good",       {"drop_after": "data_prompt"},            "refuse"),
    # Hang-shaped personas: they never complete on their own, so only the client's own
    # timeout ends them. Driven against the short-budget endpoints in main.cpp, whose
    # 5s budgets are the engine's floor, rather than the 8s every other persona gets
    ("flood_greeting",      8110, "good",       {"flood_at": "greeting"},                 "hang"),
    ("flood_ehlo2",         8111, "good",       {"flood_at": "ehlo2"},                    "hang"),
    ("huge_line_greeting",  8112, "good",       {"huge_line_at": "greeting"},             "hang"),
    ("huge_line_ehlo2",     8113, "good",       {"huge_line_at": "ehlo2"},                "hang"),
    ("slow_trickle",        8114, "good",       {"slow_trickle": "0.05"},                 "hang"),
    ("silent_data",         8115, "good",       {"silent_after": "DATA"},                 "hang"),
]

# One persona per fixed port, same shape as SMTP_PERSONAS but without a cert column:
# no Postgres persona needs one, every SSL-verdict fault is refused by the client
# before a TLS handshake would start. Ports and names are compiled into
# app/src/main.cpp's Pg_* endpoints and PgEndpointOf(), keep the two in sync.
PG_CONTROL_PORT = 8198

PG_PERSONAS = [
    ("good",                  8130, {}, "accept"),

    # Handshake message-ordering: an abrupt close at each stage before ReadyForQuery
    ("drop_startup",          8131, {"drop_after": "startup"},          "refuse"),
    ("drop_auth_challenge",   8132, {"drop_after": "auth_challenge"},   "refuse"),
    ("drop_auth_final",       8133, {"drop_after": "auth_final"},       "refuse"),
    ("drop_backendkeydata",   8134, {"drop_after": "backendkeydata"},   "refuse"),
    ("drop_ready",            8135, {"drop_after": "ready"},            "refuse"),
    ("unknown_type_startup",  8136, {"unknown_type_at": "startup"},     "refuse"),

    # Resource-shaped: a flood of ParameterStatus never lets the handshake finish, an
    # oversized NoticeResponse trips FrameMessage's declared-length bound instead
    ("flood_backendkeydata",  8137, {"flood_at": "backendkeydata"},                 "hang"),
    ("huge_ready",            8138, {"huge_at": "ready", "huge_bytes": "8192"},      "refuse"),
    ("never_reply_ready",     8139, {"never_reply": "ready"},                       "hang"),

    # SSL verdict handling. CVE-2021-23214 class: plaintext spliced in right after the
    # verdict byte, before the TLS handshake even starts. A correct client never trusts
    # bytes that arrive before its own ClientHello could possibly have been answered
    ("ssl_garbage",           8140, {"ssl_verdict": "garbage"},   "refuse"),
    ("ssl_inject",            8141, {"ssl_verdict": "multibyte"}, "refuse"),
    ("ssl_reject_required",   8142, {"ssl_verdict": "N"},         "refuse"),

    # Auth downgrade: MD5, GSS, SSPI and an unpolicied cleartext request all have to
    # fail closed, with no fallback path that would accept them
    ("wrong_auth_md5",        8143, {"wrong_auth": "md5"},   "refuse"),
    ("wrong_auth_gss",        8144, {"wrong_auth": "gss"},   "refuse"),
    ("wrong_auth_sspi",       8145, {"wrong_auth": "sspi"},  "refuse"),
    ("wrong_auth_cleartext",  8146, {"wrong_auth": "cleartext"},                      "refuse"),
    ("cleartext_allowed",     8147, {"wrong_auth": "cleartext", "allow_cleartext": "1"}, "accept"),

    # SCRAM-SHA-256 exchange: channel-binding-only offers are refused (no channel
    # binding support), and each of the exchange's five checked fields is corrupted
    # in turn
    ("scram_offer_empty",     8148, {"scram_offer": "empty"},      "refuse"),
    ("scram_offer_plus_only", 8149, {"scram_offer": "plus_only"},  "refuse"),
    ("scram_offer_garbage",   8150, {"scram_offer": "garbage"},    "refuse"),
    ("scram_bad_nonce",       8151, {"mangle": "scram_nonce"},               "refuse"),
    ("scram_iter_zero",       8152, {"mangle": "scram_iterations_zero"},     "refuse"),
    ("scram_iter_over",       8153, {"mangle": "scram_iterations_over"},     "refuse"),
    ("scram_bad_salt",        8154, {"mangle": "scram_salt"},                "refuse"),
    ("scram_bad_signature",   8155, {"mangle": "scram_signature"},           "refuse"),

    # Post-handshake: a clean connection for the wire/result/type/stream/cache phases
    # to run PGAUDIT_* queries and multi-statement sessions against
    ("wire_stream",           8156, {}, "accept"),
    ("cache_epoch_feature",   8157, {"force_error_at": "2:0A000:relation dropped"},     "accept"),
    ("cache_epoch_badname",   8158, {"force_error_at": "2:26000:invalid statement name"}, "accept"),
    # Handshake completes and joins the pool normally; only the query itself hangs, so
    # onAbort has a real backendPid to cancel rather than a still-connecting slot
    ("cancel_probe",          8159, {"never_reply": "query"}, "hang"),

    ("resource_huge_row",     8160, {}, "refuse"),
    ("resource_slow_trickle", 8161, {"slow_trickle": "0.05"}, "hang"),

    # applicationName carries an embedded NUL (main.cpp's Pg_startup_nul_inject
    # config). The mock just records the raw bytes it received, it does not need a
    # fault of its own
    ("startup_nul_inject",    8162, {}, "accept"),
    # First TCP connection refused, second accepted normally; main.cpp's
    # Pg_reconnect_isolation gives the client 2 reconnect attempts to get there
    ("reconnect_isolation",   8163, {"fail_first_connect": "1"}, "accept"),
    # A mechanism list offering both PLUS and plain SCRAM-SHA-256, the shape a real
    # channel-binding-capable server sends. Plain has to still be picked and used
    ("scram_mixed",           8164, {"scram_offer": "mixed"}, "accept"),
    # RFC 5802's e=<reason> server-final path, never exercised before: the server
    # itself rejects the exchange for a reason unrelated to proof correctness
    ("scram_server_error",    8165, {"mangle": "scram_server_error"}, "refuse"),
    # CVE-2024-10977 class: a full ErrorResponse mid-handshake carrying
    # attacker-shaped content (ANSI escapes, CR/LF), from a peer not yet trusted
    ("error_at_handshake",    8166, {"error_at": "auth_challenge"}, "refuse"),
]

# Mocks
class HttpMock:
    """http_upstream.py: a byte oracle plus the counters this suite reads back."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.proc = None

    def start(self):
        cmd = [sys.executable, os.path.join(HERE, "http_upstream.py"),
               "--host", self.cfg.host, "--port", str(self.cfg.http_port)]
        term.log("http-mock", "starting: %s" % " ".join(cmd))
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

        for _ in range(50):
            if self.ping():
                term.log("http-mock", term.green("up on %s:%d" % (self.cfg.host, self.cfg.http_port)))
                return
            time.sleep(0.1)

        raise RuntimeError("HTTP mock never came up on port %d" % self.cfg.http_port)

    def _ctl(self, path):
        raw = net.send(self.cfg.host, self.cfg.http_port, net.request("GET", path),
                       rtimeout=2.0, ctimeout=2.0)
        return net.body(raw) if raw else b""

    def _count(self, path):
        try:
            return int(self._ctl(path))
        except ValueError:
            return -1

    def ping(self):
        return self._ctl("/ctl/ping") == b"pong"

    def stage(self, stage_id, blob, keep, mode="whole", arg=0):
        """Parks raw response bytes the mock replays when WFX fetches /raw/<stage_id>."""
        net.send(self.cfg.host, self.cfg.http_port,
                 net.request("POST", "/ctl/stage",
                             {"X-Id": stage_id, "X-Keep": "1" if keep else "0",
                              "X-Mode": mode, "X-Arg": str(arg)}, blob))

    def coalesce_reset(self):
        self._ctl("/ctl/coalesce/reset")

    def coalesce_hits(self):
        """Backend calls on /coalesce* since the last reset, the dedup evidence."""
        return self._count("/ctl/coalesce/count")

    def idle_conns(self):
        """Accepted connections minus served requests, so connections opened and never used."""
        return self._count("/ctl/idleconns")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

class SmtpMock:
    """smtp_upstream.py: one listener per persona, plus a line-based control port."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.proc = None
        self.ports = {name: port for name, port, cert, opts, expect in SMTP_PERSONAS
                      if smtp_cert_available(cfg, cert)}

    def start(self):
        cmd = [sys.executable, os.path.join(HERE, "smtp_upstream.py"), "--host", self.cfg.host,
               "--control-port", str(SMTP_CONTROL_PORT)]
        for name, port, cert, opts, expect in SMTP_PERSONAS:
            if not smtp_cert_available(self.cfg, cert):
                continue
            spec = "name=%s,port=%d,cert=%s,key=%s" % (
                name, port, os.path.join(self.cfg.cert_dir, cert + ".pem"),
                os.path.join(self.cfg.cert_dir, cert + "-key.pem"))
            for key, value in opts.items():
                spec += ",%s=%s" % (key, value)
            cmd += ["--listen", spec]

        term.log("smtp-mock", "starting %d SMTP listeners" % len(self.ports))
        # Stream the mock's own output rather than discarding it: a listener thread that
        # fails to bind or load its cert prints a traceback, and swallowing that makes a
        # broken fixture indistinguishable from a real WFX bug
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
        threading.Thread(target=self._drain, daemon=True).start()

        good_port = self.ports.get("good", 8100)
        deadline = time.time() + 5.0
        while time.time() < deadline:
            try:
                sock = socket.create_connection((self.cfg.host, good_port), timeout=1.0)
                sock.close()
                term.log("smtp-mock", term.green("up on %d listeners" % len(self.ports)))
                return
            except OSError:
                time.sleep(0.1)

        raise RuntimeError("SMTP mock never came up on :%d, see [smtp-mock] above" % good_port)

    def _drain(self):
        try:
            for line in self.proc.stdout:
                line = line.rstrip()
                if line:
                    print("%s %s" % (term.cyan("[smtp-mock]"), line), flush=True)
        except (OSError, ValueError):
            pass

    def _control(self, command):
        try:
            sock = socket.create_connection((self.cfg.host, SMTP_CONTROL_PORT), timeout=3.0)
            sock.sendall((command + "\n").encode())
            buf = b""
            while b"\n" not in buf:
                data = sock.recv(65536)
                if not data:
                    break
                buf += data
            sock.close()
            return json.loads(buf.split(b"\n", 1)[0].decode())
        except (OSError, ValueError):
            return {}

    def stats(self, persona):
        """The mock's own side of the exchange: handshakes, hs_fail, auth_ok, auth_fail, bodies."""
        return self._control("STATS %s" % persona)

    def reset(self, persona):
        self._control("RESET %s" % persona)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

class PgMock:
    """postgres_upstream.py: one listener per persona, plus a line-based control port."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.proc = None
        self.ports = {name: port for name, port, opts, expect in PG_PERSONAS}

    def start(self):
        cmd = [sys.executable, os.path.join(HERE, "postgres_upstream.py"), "--host", self.cfg.host,
               "--control-port", str(PG_CONTROL_PORT)]
        for name, port, opts, expect in PG_PERSONAS:
            spec = "name=%s,port=%d" % (name, port)
            for key, value in opts.items():
                spec += ",%s=%s" % (key, value)
            cmd += ["--listen", spec]

        term.log("pg-mock", "starting %d Postgres listeners" % len(self.ports))
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
        threading.Thread(target=self._drain, daemon=True).start()

        good_port = self.ports.get("good", 8130)
        deadline = time.time() + 5.0
        while time.time() < deadline:
            try:
                sock = socket.create_connection((self.cfg.host, good_port), timeout=1.0)
                sock.close()
                term.log("pg-mock", term.green("up on %d listeners" % len(self.ports)))
                return
            except OSError:
                time.sleep(0.1)

        raise RuntimeError("Postgres mock never came up on :%d, see [pg-mock] above" % good_port)

    def _drain(self):
        try:
            for line in self.proc.stdout:
                line = line.rstrip()
                if line:
                    print("%s %s" % (term.cyan("[pg-mock]"), line), flush=True)
        except (OSError, ValueError):
            pass

    def _control(self, command):
        try:
            sock = socket.create_connection((self.cfg.host, PG_CONTROL_PORT), timeout=3.0)
            sock.sendall((command + "\n").encode())
            buf = b""
            while b"\n" not in buf:
                data = sock.recv(65536)
                if not data:
                    break
                buf += data
            sock.close()
            return json.loads(buf.split(b"\n", 1)[0].decode())
        except (OSError, ValueError):
            return {}

    def stats(self, persona):
        """The mock's own side of the exchange: handshakes, hs_fail, auth_ok, auth_fail,
        queries, cancels."""
        return self._control("STATS %s" % persona)

    def reset(self, persona):
        self._control("RESET %s" % persona)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

# Certificates (SMTP STARTTLS trust)
def openssl(args):
    return subprocess.run(["openssl"] + args, capture_output=True, text=True)

def smtp_cert_available(cfg, cert):
    return cert in cfg.smtp_certs

def ensure_smtp_certs(cfg):
    """Generates the STARTTLS certs: good, selfsigned, wronghost and expired.

    Plain openssl throughout, because STARTTLS trust needs a cert this suite's own CA
    signed, not a browser-trusted one. Every other persona reuses the 'good' cert,
    since they exercise protocol behaviour rather than certificate trust.
    """
    os.makedirs(cfg.cert_dir, exist_ok=True)

    def path(name):
        return os.path.join(cfg.cert_dir, name)

    ca_made = openssl(["req", "-x509", "-newkey", "rsa:2048", "-nodes",
                       "-keyout", path("ca-key.pem"), "-out", path("ca.pem"),
                       "-days", "365", "-subj", "/CN=client-audit-test-ca",
                       "-addext", "basicConstraints=critical,CA:true",
                       "-addext", "keyUsage=critical,keyCertSign,cRLSign"]).returncode == 0
    if not ca_made:
        raise RuntimeError("could not generate the audit's throwaway CA, is openssl broken?")
    cfg.smtp_ca_path = path("ca.pem")

    def sign(name, subject, san, days="365"):
        csr, ext = path(name + ".csr"), path(name + "-ext.cnf")
        with open(ext, "w") as f:
            f.write("subjectAltName=%s\n" % san)

        made_csr = openssl(["req", "-new", "-newkey", "rsa:2048", "-nodes",
                            "-keyout", path(name + "-key.pem"), "-subj", subject,
                            "-out", csr]).returncode == 0
        signed = made_csr and openssl(["x509", "-req", "-in", csr, "-CA", path("ca.pem"),
                                       "-CAkey", path("ca-key.pem"), "-CAcreateserial",
                                       "-out", path(name + ".pem"), "-days", days,
                                       "-extfile", ext]).returncode == 0
        return signed and os.path.exists(path(name + ".pem"))

    available = set()
    if sign("good", "/CN=127.0.0.1", "IP:127.0.0.1"):
        available.add("good")
    if sign("wronghost", "/CN=evil.example", "DNS:evil.example"):
        available.add("wronghost")
    # -days -1 backdates notAfter to yesterday, which is portable across OpenSSL
    # versions, unlike -not_before/-not_after which are OpenSSL 3.0 and later only
    if sign("expired", "/CN=127.0.0.1", "IP:127.0.0.1", days="-1"):
        available.add("expired")
    if openssl(["req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", path("selfsigned-key.pem"), "-out", path("selfsigned.pem"),
                "-days", "365", "-subj", "/CN=127.0.0.1",
                "-addext", "subjectAltName=IP:127.0.0.1"]).returncode == 0:
        available.add("selfsigned")

    term.log("smtp-certs", term.green("certs available: %s" % ", ".join(sorted(available))))
    cfg.smtp_certs = available
    if "good" not in available:
        raise RuntimeError("could not generate the audit's 'good' STARTTLS cert")

def patch_outbound_ca(cfg):
    """Points outbound_ca_path at the throwaway CA, so the 'good' persona is trusted.

    The HTTP mock is plaintext, so nothing else in this suite reads that setting.
    """
    toml = os.path.join(cfg.app_dir, "config", "wfx.local.toml")
    with open(toml) as f:
        text = f.read()

    text = re.sub(r'(?m)^(\s*outbound_ca_path\s*=\s*)"[^"]*"',
                  lambda m: m.group(1) + '"%s"' % cfg.smtp_ca_path, text)

    with open(toml, "w") as f:
        f.write(text)

    term.log("patch", "outbound TLS CA trust -> %s" % cfg.smtp_ca_path)

# Driving WFX: HTTP
def _json(cfg, method, path, headers=None, payload=b"", rtimeout=15.0):
    return net.get_json(cfg.host, cfg.port, method, path, headers, payload, rtimeout=rtimeout)

def http_call(cfg, path, ep="default", method="GET", want=None, fwd=None, x_body=None,
              rtimeout=8.0):
    """One outbound HttpEndpoint call, reflected back as JSON.

    rtimeout has to exceed the outbound budget for the slow and connect-failure
    endpoints: WFX does not answer /call until the outbound request resolves, which
    for those is up to ~10s (a 5s budget plus one 5s timer tick).
    """
    headers = {"X-Ep": ep, "X-Method": method, "X-Path": path}
    if want:
        headers["X-Want"] = want
    if fwd:
        headers["X-Fwd"] = fwd
    if x_body is not None:
        headers["X-Body"] = x_body

    return _json(cfg, "GET", "/call", headers, rtimeout=rtimeout)

def http_staged(cfg, mock, blob, ep="default", keep=True, mode="whole", arg=0):
    """Replays `blob` verbatim as the upstream response, then drives one call at it.

    keep defaults to True, so the mock holds the connection open after replaying a
    COMPLETE self-delimiting response, which is what the client does too: it pools the
    keep-alive slot. A mock that closed instead while the client pooled would leave the
    NEXT staged request reusing a dead socket. EOF, truncation and close-delimited
    vectors pass keep=False explicitly, because the close is the thing under test.
    """
    stage_id = "s%d" % next(_stage_ids)
    mock.stage(stage_id, blob, keep, mode, arg)
    return http_call(cfg, "/raw/%s" % stage_id, ep=ep)

def http_split(cfg, mock, blob, offset=0, ep="default", keep=True):
    """Delivers `blob` as two sends split at byte `offset`, 0 meaning the midpoint."""
    return http_staged(cfg, mock, blob, ep=ep, keep=keep, mode="split", arg=offset)

def http_drip(cfg, mock, blob, piece=1, ep="default", keep=True):
    """Delivers `blob` `piece` bytes at a time, one recv() boundary per piece."""
    return http_staged(cfg, mock, blob, ep=ep, keep=keep, mode="drip", arg=piece)

def http_inject(cfg, field, payload, ep="default"):
    """Feeds the serializer raw bytes the inbound header parser would never allow.

    field is "path" or "header"; the payload travels as the POST body, which is the
    only way to get a genuine CR/LF/NUL past WFX's own inbound parser.
    """
    return _json(cfg, "POST", "/inject", {"X-Ep": ep, "X-Inject": field}, payload, rtimeout=8.0)

def http_request_head(cfg, fwd=None, fwd2=None, fwd3=None, method="GET", x_body=None):
    """The exact request head WFX put on the wire, with every CR and LF shown as '|'."""
    headers = {"X-Ep": "default", "X-Method": method, "X-Path": "/reflectraw"}
    if fwd:
        headers["X-Fwd"] = fwd
    if fwd2:
        headers["X-Fwd2"] = fwd2
    if fwd3:
        headers["X-Fwd3"] = fwd3
    if x_body is not None:
        headers["X-Body"] = x_body

    return (_json(cfg, "GET", "/call", headers, rtimeout=8.0) or {}).get("body")

def count_ci(head, needle):
    return head.lower().count(needle.lower())

# Driving WFX: SMTP
#
# The rtimeout defaults are generous on purpose: a hang-shaped persona only resolves
# once WFX's own connectTimeoutSeconds or requestTimeoutSeconds fires, and that timer
# ticks every 5s (INVOKE_TIMEOUT_COOLDOWN), so the observed wait runs up to the
# budget plus one tick
def _smtp_headers(persona, from_addr, from_name, to_addr, to_name, subject, reply_to):
    headers = {"X-Persona": persona}
    for name, value in (("X-From", from_addr), ("X-FromName", from_name), ("X-To", to_addr),
                        ("X-ToName", to_name), ("X-Subject", subject), ("X-ReplyTo", reply_to)):
        if value is not None:
            headers[name] = value
    return headers

def smtp_send(cfg, persona="good", from_addr=None, from_name=None, to_addr=None, to_name=None,
              subject=None, reply_to=None, body=b"hello", rtimeout=20.0):
    """A full transaction driven one command at a time, so a failure names its stage."""
    return _json(cfg, "POST", "/smtp/send",
                 _smtp_headers(persona, from_addr, from_name, to_addr, to_name, subject, reply_to),
                 body, rtimeout)

def smtp_send_mail(cfg, persona="good", body=b"hello", rtimeout=20.0):
    """The same transaction through SmtpEndpoint::SendMail, one nested coroutine."""
    return _json(cfg, "POST", "/smtp/send-mail",
                 _smtp_headers(persona, None, None, None, None, None, None), body, rtimeout)

def smtp_inject(cfg, field, payload, rtimeout=20.0):
    """Splices a hostile payload into one caller-supplied field of the transaction."""
    return _json(cfg, "POST", "/smtp/inject", {"X-Field": field}, payload, rtimeout)

# Driving WFX: Postgres
#
# rtimeout defaults are generous for the same reason as SMTP's: a hang-shaped persona
# only resolves once WFX's own connectTimeoutSeconds or requestTimeoutSeconds fires
def pg_query(cfg, persona="good", sql="SELECT 1", param=None, rtimeout=15.0):
    """One pooled Postgres query, reflected back as JSON.

    param, when given, is bound as $1 instead of leaving the query parameterless,
    so a hostile value travels through Bind rather than the SQL text.
    """
    headers = {"X-Persona": persona}
    if param is not None:
        headers["X-Param"] = param
    return _json(cfg, "POST", "/pg/query", headers, sql.encode(), rtimeout)

def pg_session(cfg, persona="good", statements=(), isolation="read_committed", finish="commit",
               rtimeout=15.0):
    """A pinned session: Begin, one statement per line, then Commit or Rollback."""
    headers = {"X-Persona": persona, "X-Isolation": isolation, "X-Finish": finish}
    return _json(cfg, "POST", "/pg/session", headers, "\n".join(statements).encode(), rtimeout)

def pg_stream(cfg, persona="wire_stream", sql="SELECT PGAUDIT_STREAM", chunk_rows=2, rtimeout=15.0):
    """A chunked Postgres read through PostgresEndpoint::Stream."""
    headers = {"X-Persona": persona, "X-ChunkRows": str(chunk_rows)}
    return _json(cfg, "POST", "/pg/stream", headers, sql.encode(), rtimeout)

def abandon_pg_query(cfg, persona, sql=b"SELECT 1", hold=0.15):
    """Sends /pg/query and drops the connection before WFX can answer, the same
    abandon-and-inspect shape endpoint_audit uses to reach onAbort."""
    net.send_and_abandon(cfg.host, cfg.port, net.request("POST", "/pg/query", {"X-Persona": persona}, sql),
                         hold=hold)

# Metrics
def metrics(cfg):
    return _json(cfg, "GET", "/metrics", rtimeout=4.0) or {}

def metric(snapshot, field):
    """Sums one counter across every endpoint slot.

    Several instances share a host, so a slot cannot be mapped back to one instance;
    the aggregate delta either side of a driven call is what is assertable.
    """
    return sum(e.get(field, 0) for e in snapshot.get("endpoints", []))

def latency_metric(snapshot, field):
    return sum((e.get("latency") or {}).get(field, 0) for e in snapshot.get("endpoints", []))

# Predicates
def is_ok(r, status=None, body=None):
    """WFX answered, the outbound call succeeded, and the response matches."""
    if not r or r.get("ep") != EP_SUCCESS:
        return False
    if status is not None and r.get("status") != status:
        return False
    if body is not None and r.get("body") != body:
        return False
    return True

def is_err(r):
    """WFX answered, but the outbound call failed cleanly."""
    return bool(r) and r.get("ep") != EP_SUCCESS

def is_errc(r, code):
    return bool(r) and r.get("ep") == code

def answered(r):
    """WFX produced a well-formed answer at all, so it neither crashed nor hung."""
    return r is not None

def smtp_ok(r, stage="done"):
    return bool(r) and r.get("ep") == EP_SUCCESS and r.get("stage") == stage

def wfx_healthy(cfg):
    raw = net.send(cfg.host, cfg.port, net.request("GET", "/health"), rtimeout=2.0, ctimeout=2.0)
    return bool(raw) and net.status(raw) == 200

# Building hostile responses
def response(status_line, headers=(), body=b""):
    if isinstance(body, str):
        body = body.encode("latin-1")
    head = status_line + "\r\n" + "".join("%s\r\n" % h for h in headers) + "\r\n"
    return head.encode("latin-1") + body

def chunked(chunks):
    return b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunks

# Concurrency
def in_parallel(fn, n, stagger=0.01):
    """Runs fn(i) for i in range(n) at once, results in submission order.

    The small stagger spreads the inbound connection burst, so WFX's accept path is
    not hit by n simultaneous SYNs, which used to drop a few of them as None. At 10ms
    each it still lands well inside the 300ms coalesce window, so dedup still engages.
    """
    out = [None] * n

    def worker(i):
        out[i] = fn(i)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(n)]
    for thread in threads:
        thread.start()
        if stagger:
            time.sleep(stagger)
    for thread in threads:
        thread.join()

    return out

def pool_stays_clean(cfg, ep, n=10):
    """After a hostile response on `ep`, every following /ok must come back pristine."""
    return all(is_ok(http_call(cfg, "/ok", ep=ep), 200, "hello") for _ in range(n))

# PHASE: http_framing
def phase_http_framing(ctx):
    cfg = ctx.cfg
    p = ctx.phase("http_framing")

    # The whole legal response matrix has to be accepted before any of the hostile
    # phases below mean anything
    p.check("content-length small",        is_ok(http_call(cfg, "/ok"), 200, "hello"))
    p.check("content-length zero",         is_ok(http_call(cfg, "/empty"), 200, ""))
    p.check("content-length 1000",
            (lambda r: is_ok(r, 200) and r.get("bodylen") == 1000)(http_call(cfg, "/cl/1000")))
    p.check("chunked single",              is_ok(http_call(cfg, "/chunked/1"), 200, "[c0]"))
    p.check("chunked multi (5)",
            is_ok(http_call(cfg, "/chunked/5"), 200, "[c0][c1][c2][c3][c4]"))
    p.check("chunked extension ignored",   is_ok(http_call(cfg, "/chunked-ext"), 200, "abc"))
    p.check("chunked trailer discarded",   is_ok(http_call(cfg, "/chunked-trailer"), 200, "xy"))
    p.check("close-delimited body",        is_ok(http_call(cfg, "/close"), 200, "closebody"))
    p.check("http/1.0 close-delimited",    is_ok(http_call(cfg, "/http10"), 200, "ten"))
    p.check("HEAD is bodyless",
            is_ok(http_call(cfg, "/evil/headbody", method="HEAD"), 200, ""))
    p.check("1xx: 100 then 200",           is_ok(http_call(cfg, "/continue"), 200, "after"))
    p.check("1xx: eight informational lines", is_ok(http_call(cfg, "/continue/8"), 200, "done"))
    p.check("response header retrievable",
            (lambda r: is_ok(r, 200) and r.get("hdr") == "alpha")(
                http_call(cfg, "/ok", want="X-Mark")))
    p.check("header lookup is case-insensitive",
            (lambda r: is_ok(r, 200) and r.get("hdr") == "alpha")(
                http_call(cfg, "/ok", want="x-mark")))

    for code in (200, 201, 202, 301, 400, 404, 418, 500, 599, 999):
        p.check("status passthrough %d" % code, is_ok(http_call(cfg, "/status/%d" % code), code))

    p.check("status 204 has no body", is_ok(http_call(cfg, "/status/204"), 204, ""))
    p.check("status 304 has no body", is_ok(http_call(cfg, "/status/304"), 304, ""))

# PHASE: http_statusline
def phase_http_statusline(ctx):
    cfg, mock = ctx.cfg, ctx.http_mock
    p = ctx.phase("http_statusline")

    # Each line is replayed as "<line>\r\nContent-Length: 0\r\n\r\n"
    accepted = [
        ("valid 200",          "HTTP/1.1 200 OK"),
        ("valid no reason",    "HTTP/1.1 200"),
        ("valid 999",          "HTTP/1.1 999 X"),
        ("valid 000",          "HTTP/1.1 000 X"),
        ("http/1.0",           "HTTP/1.0 200 OK"),
        ("valid 042 leadzero", "HTTP/1.1 042 X"),
        ("reason with digits", "HTTP/1.1 200 200 OK"),
        ("no-space reason",    "HTTP/1.1 200OK"),   # the first 12 bytes are valid
        ("long reason",        "HTTP/1.1 200 " + "R" * 300),
        ("reason w/ colon",    "HTTP/1.1 200 O:K"),
        ("reason w/ tab-in",   "HTTP/1.1 200 O\tK"),
        ("tab after code",     "HTTP/1.1 200\tOK"), # the separator before the reason is not validated
    ]
    for name, line in accepted:
        blob = (line + "\r\nContent-Length: 0\r\n\r\n").encode("latin-1")
        p.check("line ok: %s" % name, is_ok(http_staged(cfg, mock, blob)))

    rejected = [
        ("http/2.0",           "HTTP/2.0 200 OK"),
        ("http/3.0",           "HTTP/3.0 200 OK"),
        ("http/1.2 minor",     "HTTP/1.2 200 OK"),
        ("http/0.9",           "HTTP/0.9 200 OK"),
        ("http/1.9 minor",     "HTTP/1.9 200 OK"),
        ("major non-digit",    "HTTP/x.1 200 OK"),
        ("lowercase proto",    "http/1.1 200 OK"),
        ("mixed-case proto",   "HTTp/1.1 200 OK"),
        ("comma version",      "HTTP/1,1 200 OK"),
        ("colon version",      "HTTP/1:1 200 OK"),
        ("no dot version",     "HTTP/11 200 OK"),
        ("underscore version", "HTTP/1_1 200 OK"),
        ("dot shifted",        "HTTP/1.10 200 OK"),
        ("backslash proto",    "HTTP\\1.1 200 OK"),
        ("ICY (shoutcast)",    "ICY 200 OK"),
        ("RTSP proto",         "RTSP/1.0 200 OK"),
        ("leading space",      " HTTP/1.1 200 OK"),
        ("prefix junk",        "XHTTP/1.1 200 OK"),
        ("short line",         "HTTP/1.1 20"),
        ("bare proto",         "HTTP/1.1"),
        ("empty line",         ""),
        ("2-digit code",       "HTTP/1.1 99 X"),
        ("1-digit code",       "HTTP/1.1 2 X"),
        ("alpha code",         "HTTP/1.1 abc X"),
        ("plus code",          "HTTP/1.1 +20 X"),
        ("minus code",         "HTTP/1.1 -20 X"),
        ("spaced code",        "HTTP/1.1 20 0 X"),
        ("double space",       "HTTP/1.1  200 X"),
        ("tab separator",      "HTTP/1.1\t200 X"),
        ("no space after ver", "HTTP/1.1_200 X"),
        ("http/1.11 minor",    "HTTP/1.11 200 OK"),
        ("http/10.1 major",    "HTTP/10.1 200 OK"),
        ("empty version",      "HTTP/. 200 OK"),
        ("space in version",   "HTTP/1 .1 200 OK"),
        ("trailing dot",       "HTTP/1. 200 OK"),
        ("hex code 0x",        "HTTP/1.1 0x0 X"),
        ("code with space",    "HTTP/1.1 2 0 0 X"),
        ("nul in code",        "HTTP/1.1 2\x000 X"),
        ("just HTTP/",         "HTTP/"),
        ("HTTP no slash",      "HTTP1.1 200 OK"),
        ("SIP proto",          "SIP/2.0 200 OK"),
        ("gopher junk",        "gopher://x 200"),
        ("leading tab",        "\tHTTP/1.1 200 OK"),
        ("double proto",       "HTTP/1.1 HTTP/1.1 200"),
        ("code then letters",  "HTTP/1.1 20a X"),
        ("negative version",   "HTTP/-1.1 200 OK"),
    ]
    for name, line in rejected:
        r = http_staged(cfg, mock, (line + "\r\n\r\n").encode("latin-1"))
        p.check("line bad: %s" % name, is_err(r), "expected an error, got %r" % r)

# PHASE: http_headers
def phase_http_headers(ctx):
    cfg, mock = ctx.cfg, ctx.http_mock
    p = ctx.phase("http_headers")

    def with_header(block, body=b"hi", content_length=None):
        length = len(body) if content_length is None else content_length
        head = "HTTP/1.1 200 OK\r\n%s\r\nContent-Length: %d\r\n\r\n" % (block, length)
        return head.encode("latin-1") + body

    accepted = [
        ("empty value",          with_header("X-Empty:")),
        ("value OWS trimmed",    with_header("X-A:    v   ")),
        ("value tab-trimmed",    with_header("X-A:\tv\t")),
        ("value all-spaces",     with_header("X-A:      ")),
        ("value many colons",    with_header("X-A: a:b:c:d")),
        ("Connection: close",    with_header("Connection: close")),
        ("Connection keep,close", with_header("Connection: keep-alive, close")),
        ("Connection Close case", with_header("Connection: Close")),
        ("Connection close,keep", with_header("Connection: close, keep-alive")),
        ("Connection tab tokens", with_header("Connection: \tkeep-alive\t,\tclose\t")),
        ("Connection closed != close", with_header("Connection: closed")),
        ("dup CL equal",         b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nhi"),
        ("dup CL triple equal",  b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n"
                                 b"Content-Length: 2\r\n\r\nhi"),
        ("CL leading zeros",     b"HTTP/1.1 200 OK\r\nContent-Length: 0002\r\n\r\nhi"),
        ("CL trailing OWS",      b"HTTP/1.1 200 OK\r\nContent-Length: 2 \r\n\r\nhi"),
        ("many benign headers",  ("HTTP/1.1 200 OK\r\n"
                                  + "".join("X-H%d: v%d\r\n" % (i, i) for i in range(30))
                                  + "Content-Length: 2\r\n\r\nhi").encode("latin-1")),
    ]
    for name, blob in accepted:
        p.check("hdr ok: %s" % name, is_ok(http_staged(cfg, mock, blob), 200, "hi"))

    p.check("hdr ok: dup TE chunked",
            is_ok(http_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                                         b"Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"),
                  200, "abc"))

    # Framing and smuggling refusals. The security-marked ones are the shapes a request
    # smuggler actually uses to make two intermediaries disagree about where a message ends
    smuggling = ("obs-fold continuation", "space before colon", "CL then TE", "TE then CL",
                 "dup CL differ", "TE chunked,gzip", "TE gzip,chunked", "TE then CL differ",
                 "CL then TE case", "dup CL differ 2vs0", "TE Chunked,chunked", "TE chunked;q=0",
                 "blank-ish fold line")
    rejected = [
        ("obs-fold continuation", b"HTTP/1.1 200 OK\r\nX-Fold: a\r\n b\r\nContent-Length: 2\r\n\r\nhi"),
        ("leading-tab fold",      b"HTTP/1.1 200 OK\r\n\tX: y\r\nContent-Length: 2\r\n\r\nhi"),
        ("no colon",              with_header("NoColonHeader")),
        ("empty name",            with_header(": value")),
        ("space before colon",    with_header("Name : value")),
        ("tab before colon",      with_header("Name\t: value")),
        ("dup CL differ",         b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nhi"),
        ("CL then TE",            b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\nhi"),
        ("TE then CL",            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\nhi"),
        ("CL non-numeric",        with_header("Content-Length: 2x", content_length=None)),
        ("CL hex",                b"HTTP/1.1 200 OK\r\nContent-Length: 0x2\r\n\r\nhi"),
        ("CL plus",               b"HTTP/1.1 200 OK\r\nContent-Length: +2\r\n\r\nhi"),
        ("CL minus",              b"HTTP/1.1 200 OK\r\nContent-Length: -2\r\n\r\nhi"),
        ("CL internal space",     b"HTTP/1.1 200 OK\r\nContent-Length: 2 2\r\n\r\nhi"),
        ("CL empty",              b"HTTP/1.1 200 OK\r\nContent-Length:\r\n\r\nhi"),
        ("CL overflow u64",       b"HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999999\r\n\r\nhi"),
        ("TE gzip",               b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nx"),
        ("TE chunked,gzip",       b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\nx"),
        ("TE gzip,chunked",       b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\nx"),
        ("TE x-chunked",          b"HTTP/1.1 200 OK\r\nTransfer-Encoding: xchunked\r\n\r\nx"),
        ("TE identity",           b"HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\n\r\nx"),
        ("TE chunked;q=0",        b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;q=0\r\n\r\nx"),
        ("TE Chunked,chunked",    b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, chunked\r\n\r\nx"),
        ("TE deflate",            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate\r\n\r\nx"),
        ("TE then CL differ",     b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\nhi"),
        ("CL then TE case",       b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\ntransfer-encoding: CHUNKED\r\n\r\nhi"),
        ("dup CL differ 2vs0",    b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 0\r\n\r\nhi"),
        ("CL with plus-space",    b"HTTP/1.1 200 OK\r\nContent-Length: + 2\r\n\r\nhi"),
        ("CL float",              b"HTTP/1.1 200 OK\r\nContent-Length: 2.0\r\n\r\nhi"),
        ("CL binary 0b10",        b"HTTP/1.1 200 OK\r\nContent-Length: 0b10\r\n\r\nhi"),
        ("CL comma sep",          b"HTTP/1.1 200 OK\r\nContent-Length: 2,2\r\n\r\nhi"),
        ("CL leading space only", b"HTTP/1.1 200 OK\r\nContent-Length:    \r\n\r\nhi"),
        ("blank-ish fold line",   b"HTTP/1.1 200 OK\r\n \r\nContent-Length: 2\r\n\r\nhi"),
        ("only colon header",     b"HTTP/1.1 200 OK\r\n:\r\nContent-Length: 2\r\n\r\nhi"),
        ("double colon empty nm", b"HTTP/1.1 200 OK\r\n::v\r\nContent-Length: 2\r\n\r\nhi"),
    ]
    for name, blob in rejected:
        r = http_staged(cfg, mock, blob)
        p.check("hdr bad: %s" % name, is_err(r), "expected an error, got %r" % r,
                security=name in smuggling)

    # Transfer-Encoding spellings that ARE chunked, so they need a real chunked body
    for name, spelling in (("Chunked case", "Chunked"), ("CHUNKED case", "CHUNKED"),
                           ("leading space", " chunked"), ("trailing space", "chunked "),
                           ("tab-wrapped", "\tchunked\t"), ("cHuNkEd", "cHuNkEd"),
                           ("spaces both", "   chunked   ")):
        blob = ("HTTP/1.1 200 OK\r\nTransfer-Encoding: %s\r\n\r\n3\r\nabc\r\n0\r\n\r\n"
                % spelling).encode("latin-1")
        p.check("hdr ok: TE %s" % name, is_ok(http_staged(cfg, mock, blob), 200, "abc"))

# PHASE: http_chunked
def phase_http_chunked(ctx):
    cfg, mock = ctx.cfg, ctx.http_mock
    p = ctx.phase("http_chunked")

    accepted = [
        ("upper hex size",      b"A\r\n0123456789\r\n0\r\n\r\n", "0123456789"),
        ("lower hex size",      b"a\r\n0123456789\r\n0\r\n\r\n", "0123456789"),
        ("leading zero size",   b"003\r\nabc\r\n0\r\n\r\n", "abc"),
        ("ext quoted",          b"3;a=\"b\"\r\nabc\r\n0\r\n\r\n", "abc"),
        ("zero-only body",      b"0\r\n\r\n", ""),
        ("many small chunks",   b"1\r\na\r\n1\r\nb\r\n1\r\nc\r\n0\r\n\r\n", "abc"),
        ("trailer then end",    b"2\r\nxy\r\n0\r\nX-T: v\r\nY-T: w\r\n\r\n", "xy"),
        ("long leadzero size",  b"000000000003\r\nabc\r\n0\r\n\r\n", "abc"),
        ("multi ext",           b"3;a=1;b=2\r\nabc\r\n0\r\n\r\n", "abc"),
        ("ext no value",        b"3;flag\r\nabc\r\n0\r\n\r\n", "abc"),
        ("uppercase hexdigs",   b"F\r\n0123456789ABCDE\r\n0\r\n\r\n", "0123456789ABCDE"),
        ("mixedcase size",      b"aB\r\n" + b"Z" * 0xAB + b"\r\n0\r\n\r\n", "Z" * 0xAB),
        ("0-size w/ ext",       b"0;done\r\n\r\n", ""),
        ("0-size w/ trailer",   b"0\r\nX-Only-Trailer: yes\r\n\r\n", ""),
        ("crlf-heavy tiny",     b"1\r\nx\r\n1\r\ny\r\n1\r\nz\r\n1\r\nw\r\n0\r\n\r\n", "xyzw"),
    ]
    for name, chunks, expect in accepted:
        p.check("chunk ok: %s" % name, is_ok(http_staged(cfg, mock, chunked(chunks)), 200, expect))

    # 200 one-byte chunks have to reassemble to exactly 200 bytes
    many = b"".join(b"1\r\n%c\r\n" % (0x61 + (i % 26)) for i in range(200)) + b"0\r\n\r\n"
    p.check("chunk 200x1-byte reassembly",
            (lambda r: is_ok(r, 200) and r.get("bodylen") == 200)(
                http_staged(cfg, mock, chunked(many))))

    # A chunk terminator the client reads loosely is how a smuggled body gets in, so
    # those vectors are the security-marked ones
    smuggling = ("term one byte off", "bad chunk terminator", "term missing lf",
                 "size w/ inner space", "data over size+eof")
    rejected = [
        ("non-hex size",         b"zz\r\nabc\r\n0\r\n\r\n"),
        ("partial-hex size",     b"1g\r\nabc\r\n0\r\n\r\n"),
        ("0x prefix size",       b"0x3\r\nabc\r\n0\r\n\r\n"),
        ("empty size ;ext",      b";ext\r\nabc\r\n0\r\n\r\n"),
        ("size overflow",        b"FFFFFFFFFFFFFFFFFF\r\nabc\r\n0\r\n\r\n"),
        ("negative size",        b"-1\r\nabc\r\n0\r\n\r\n"),
        ("space in size",        b"3 \r\nabc\r\n0\r\n\r\n"),
        ("tab in size",          b"3\t\r\nabc\r\n0\r\n\r\n"),
        ("bad chunk terminator", b"3\r\nabcX5\r\n0\r\n\r\n"),
        ("data shorter+eof",     b"5\r\nab"),
        ("empty size line",      b"\r\nabc\r\n0\r\n\r\n"),
        ("size plus sign",       b"+3\r\nabc\r\n0\r\n\r\n"),
        ("size 0x prefix up",    b"0X3\r\nabc\r\n0\r\n\r\n"),
        ("size w/ inner space",  b"3 0\r\nabc\r\n0\r\n\r\n"),
        ("size hash junk",       b"3#\r\nabc\r\n0\r\n\r\n"),
        ("size u64+1 overflow",  b"10000000000000000\r\nabc\r\n0\r\n\r\n"),
        ("term one byte off",    b"3\r\nabcX\r\n0\r\n\r\n"),
        ("term missing lf",      b"3\r\nabc\r0\r\n\r\n"),
        ("data over size+eof",   b"2\r\nabcdef"),
        ("neg then valid",       b"-0\r\n\r\n"),
        ("size dot",             b"3.\r\nabc\r\n0\r\n\r\n"),
    ]
    for name, chunks in rejected:
        # The "+eof" cases need the peer to CLOSE to signal truncation; the rest error
        # on parse, where keep-alive versus close makes no difference
        r = http_staged(cfg, mock, chunked(chunks), keep=not name.endswith("+eof"))
        p.check("chunk bad: %s" % name, is_err(r), "expected an error, got %r" % r,
                security=name in smuggling)

    # A 4 GiB single chunk: valid hex, but it has to be refused against the body cap
    p.check("chunk 4GiB is over the body cap",
            is_err(http_staged(cfg, mock, chunked(b"FFFFFFFF\r\n"))))

# PHASE: http_eof
def phase_http_eof(ctx):
    cfg, mock = ctx.cfg, ctx.http_mock
    p = ctx.phase("http_eof")

    # Truncation at every parser phase. All of these close the connection, which is
    # what makes them truncations rather than slow responses
    truncations = [
        ("partial status line",    b"HTTP/1.1 200"),
        ("status line only",       b"HTTP/1.1 200 OK\r\n"),
        ("mid headers no blank",   b"HTTP/1.1 200 OK\r\nX: y\r\n"),
        ("headers then CL nobody", b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n"),
        ("mid CL body",            b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"),
        ("mid chunk size",         b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3"),
        ("chunk size no data",     b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\n"),
        ("mid chunk data",         b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab"),
        ("chunk trailer no end",   b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nxy\r\n0\r\n"),
    ]
    for name, blob in truncations:
        p.check("eof err: %s" % name, is_err(http_staged(cfg, mock, blob, keep=False)),
                "expected an error")

    # EOF is also a legitimate end of message, when nothing else delimits the body
    p.check("eof ok: close-delimited body",
            is_ok(http_staged(cfg, mock, b"HTTP/1.1 200 OK\r\n\r\nbodybytes", keep=False),
                  200, "bodybytes"))
    p.check("eof ok: empty body then close",
            is_ok(http_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
                              keep=False), 200, ""))
    p.check("eof err: zero-byte response", is_err(http_call(cfg, "/drop")))
    p.check("eof err: connection reset", is_err(http_call(cfg, "/reset")))
    # A huge declared length with a tiny body takes the reserve path, then hits EOF
    p.check("eof err: huge Content-Length then close",
            is_err(http_staged(cfg, mock,
                               b"HTTP/1.1 200 OK\r\nContent-Length: 5000000\r\n\r\ntiny",
                               keep=False)))

    # The same truncations a byte at a time, so the incremental parser sits in each
    # phase across many recv() boundaries before hitting EOF. It still has to error
    # cleanly, never hang and never spin
    for name, blob in [
        ("mid status",     b"HTTP/1.1 200"),
        ("mid headers",    b"HTTP/1.1 200 OK\r\nX: y\r\n"),
        ("CL nobody",      b"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nabc"),
        ("mid chunk size", b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3"),
        ("mid chunk data", b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab"),
    ]:
        p.check("eof err dripped: %s" % name,
                is_err(http_drip(cfg, mock, blob, piece=1, keep=False)), "expected an error")

# PHASE: http_desync
def phase_http_desync(ctx):
    cfg = ctx.cfg
    p = ctx.phase("http_desync")

    # Each hostile response is fired at the small two-slot pool, then /ok is hammered
    # on it. If the hostile body poisoned a pooled connection, one of those will
    # misframe or hand back the smuggled bytes
    poisons = [
        ("204 with body",         "/evil/204body",  "GET"),
        ("304 with body",         "/evil/304body",  "GET"),
        ("HEAD with body",        "/evil/headbody", "HEAD"),
        ("trailing smuggle",      "/evil/trailing", "GET"),
        ("pipelined 2 responses", "/evil/pipeline", "GET"),
    ]
    for name, path, method in poisons:
        http_call(cfg, path, ep="small", method=method)
        p.check("no pool poison after: %s" % name, pool_stays_clean(cfg, "small", n=12),
                "the pooled connection was corrupted by %s" % name, security=True)

    # The smuggled body must never surface as a delivered response body
    r = http_call(cfg, "/evil/trailing")
    p.check("a smuggled trailing response is never delivered as a body",
            (r is None) or (r.get("body") != "SMUGGLE"),
            "the client delivered smuggled bytes: %r" % r, security=True)

    # Control: a legitimate 204 leaves the connection reusable
    http_call(cfg, "/status/204", ep="small")
    p.check("a legitimate 204 keeps the connection clean", pool_stays_clean(cfg, "small"))

# PHASE: http_serialize
def phase_http_serialize(ctx):
    cfg = ctx.cfg
    p = ctx.phase("http_serialize")

    host = "%s:%d" % (cfg.host, cfg.http_port)

    def reflect(fwd=None, method="GET", x_body=None):
        return http_call(cfg, "/reflect", fwd=fwd, method=method, x_body=x_body)

    # Host, Content-Length and Transfer-Encoding are the engine's to write. A caller
    # forging one has to be dropped, or a request smuggler gets to pick the framing
    for name, forged in (("Host lower", "host: evil.example"),
                         ("Host upper", "HOST: evil.example"),
                         ("Host mixed", "hOsT: evil.example")):
        r = reflect(fwd=forged)
        p.check("dedup %s" % name,
                is_ok(r, 200) and ("host=%s|" % host) in (r.get("body") or ""),
                "reflected: %r" % (r and r.get("body")), security=True)

    for name, forged in (("CL forge", "Content-Length: 999"),
                         ("CL forge case", "content-length: 999")):
        r = reflect(fwd=forged, method="POST", x_body="1234")
        p.check("dedup %s" % name, is_ok(r, 200) and "|clen=4|" in (r.get("body") or ""),
                "reflected: %r" % (r and r.get("body")), security=True)

    for name, forged in (("TE forge", "Transfer-Encoding: chunked"),
                         ("TE forge case", "transfer-encoding: chunked")):
        r = reflect(fwd=forged)
        p.check("dedup %s" % name, is_ok(r, 200) and "|te=-|" in (r.get("body") or ""),
                "reflected: %r" % (r and r.get("body")), security=True)

    r = reflect(fwd="X-Test: hello")
    p.check("a normal header is forwarded verbatim",
            is_ok(r, 200) and "|xtest=hello" in (r.get("body") or ""))

    # Content-Length correctness across a range of body sizes
    for size in (0, 1, 100, 1000):
        r = http_call(cfg, "/reflect", method="POST", x_body="z" * size)
        body = r.get("body") or "" if r else ""
        p.check("POST %d bytes carries the right Content-Length" % size,
                is_ok(r, 200) and ("|clen=%d|" % size) in body and ("|blen=%d|" % size) in body,
                "reflected: %r" % body)

    r = http_call(cfg, "/reflect", method="GET")
    p.check("a bodyless GET emits no Content-Length",
            is_ok(r, 200) and "|clen=-|" in (r.get("body") or ""))

    # A large forwarded header forces the serializer's buffer-grow retry
    big_header = "P" * 6000
    r = http_call(cfg, "/reflect", fwd="X-Test: " + big_header)
    p.check("a 6KB header survives the serializer buffer-grow retry",
            is_ok(r, 200) and ("|xtest=" + big_header) in (r.get("body") or ""))

    bigger_body = "D" * 8000
    r = http_call(cfg, "/reflect", method="POST", x_body=bigger_body)
    body = r.get("body") or "" if r else ""
    p.check("an 8KB body survives the serializer buffer-grow retry",
            is_ok(r, 200) and ("|clen=%d|" % len(bigger_body)) in body
            and ("|blen=%d|" % len(bigger_body)) in body, "reflected: %r" % body)

    # Every PUT/PATCH/POST emits Content-Length even with an empty body
    for method in ("PUT", "PATCH", "POST"):
        r = http_call(cfg, "/reflect", method=method)
        p.check("%s with an empty body still emits Content-Length: 0" % method,
                is_ok(r, 200) and "|clen=0|" in (r.get("body") or ""),
                "reflected: %r" % (r and r.get("body")))

    # Hostile path and header bytes must be refused at the serializer
    for name, payload in (("CRLF in path",  b"/a\r\nX-Smuggle: 1"),
                          ("bare LF path",  b"/a\nX-Smuggle: 1"),
                          ("bare CR path",  b"/a\rX-Smuggle: 1"),
                          ("NUL in path",   b"/a\x00b"),
                          ("CRLF at end",   b"/ok\r\n")):
        r = http_inject(cfg, "path", payload)
        p.check("reject path: %s" % name, is_errc(r, EP_SERIALIZE),
                "expected a serialize error, got %r" % r, security=True)

    for name, payload in (("CRLF in value", b"X-Evil: a\r\nX-Smuggle: b"),
                          ("bare LF value", b"X-Evil: a\nb"),
                          ("CRLF in name",  b"X-Evil\r\nX: b"),
                          ("NUL in value",  b"X-Evil: a\x00b")):
        r = http_inject(cfg, "header", payload)
        p.check("reject header: %s" % name, is_errc(r, EP_SERIALIZE),
                "expected a serialize error, got %r" % r, security=True)

    # Controls: a clean path and header still go through, and a space in a path is not
    # an injection, so it must neither be refused as one nor crash anything
    p.check("a clean injected path is accepted",
            is_ok(http_inject(cfg, "path", b"/ok"), 200, "hello"))
    p.check("a clean injected header is accepted",
            is_ok(http_inject(cfg, "header", b"X-Ok: fine"), 200, "hello"))
    p.check("a space in a path survives without smuggling",
            answered(http_inject(cfg, "path", b"/a b")))

    # Byte-exact serialization, where '|' is a CR or LF on the wire
    head = http_request_head(cfg)
    p.check("raw: the request line is exact",
            bool(head) and head.startswith("GET /reflectraw HTTP/1.1|"), "head=%r" % head)
    p.check("raw: Host appears exactly once, with the right authority",
            bool(head) and count_ci(head, "|host:") == 1 and host in head, "head=%r" % head)
    p.check("raw: a bodyless GET emits no Content-Length",
            bool(head) and count_ci(head, "content-length") == 0, "head=%r" % head)

    # A forged Host buried between two clean headers: dropped, with the clean ones kept
    # in submission order
    head = http_request_head(cfg, fwd="X-One: 1", fwd2="Host: evil.example", fwd3="X-Two: 2")
    p.check("raw: a forged Host is dropped and the clean headers keep their order",
            bool(head) and count_ci(head, "|host:") == 1 and "evil.example" not in head
            and "|X-One: 1|" in head and "|X-Two: 2|" in head
            and head.index("X-One") < head.index("X-Two"), "head=%r" % head, security=True)

    head = http_request_head(cfg, fwd="Content-Length: 999", fwd2="Transfer-Encoding: chunked",
                             method="POST", x_body="abcd")
    p.check("raw: a forged Content-Length and Transfer-Encoding are both dropped",
            bool(head) and count_ci(head, "content-length:") == 1
            and "|Content-Length: 4|" in head and count_ci(head, "transfer-encoding") == 0
            and "999" not in head, "head=%r" % head, security=True)

    head = http_request_head(cfg, fwd="A: 1", fwd2="B: 2", fwd3="C: 3")
    p.check("raw: three clean headers keep their submission order",
            bool(head) and "|A: 1|B: 2|C: 3|" in head, "head=%r" % head)

    head = http_request_head(cfg, fwd="Z-First: z")
    p.check("raw: Host is emitted before any forwarded header",
            bool(head) and head.index("|Host:") < head.index("Z-First"), "head=%r" % head)

# PHASE: http_limits
def phase_http_limits(ctx):
    cfg, mock = ctx.cfg, ctx.http_mock
    p = ctx.phase("http_limits")

    # EpSmall's caps: maxHeaderBytes 256, maxBodyBytes 1024, maxHeaderCount 8
    def header_block(total_bytes):
        pad = max(0, total_bytes - len("X-P: \r\n"))
        return ("HTTP/1.1 200 OK\r\nX-P: %s\r\nContent-Length: 0\r\n\r\n"
                % ("A" * pad)).encode("latin-1")

    def n_headers(n):
        block = "".join("X-H%d: v\r\n" % i for i in range(n))
        return ("HTTP/1.1 200 OK\r\n%sContent-Length: 0\r\n\r\n" % block).encode("latin-1")

    p.check("header block under the cap",
            is_ok(http_staged(cfg, mock, header_block(200), ep="small")))
    p.check("header block exactly at the cap",
            answered(http_staged(cfg, mock, header_block(256), ep="small")))
    p.check("header block over the cap",
            is_err(http_staged(cfg, mock, header_block(400), ep="small")))
    p.check("a single huge header line is over the cap",
            is_err(http_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nX-P: " + b"A" * 600
                                          + b"\r\nContent-Length: 0\r\n\r\n", ep="small")))

    # The Content-Length header counts toward the 8, so 7 forwarded ones is the cap
    p.check("header count at the cap of 8", is_ok(http_staged(cfg, mock, n_headers(7), ep="small")))
    p.check("header count one over the cap", is_err(http_staged(cfg, mock, n_headers(8), ep="small")))
    p.check("header count far over the cap", is_err(http_staged(cfg, mock, n_headers(20), ep="small")))

    # The status line's reason phrase counts toward maxHeaderBytes too
    p.check("a 300-byte reason phrase is over the header cap",
            is_err(http_staged(cfg, mock,
                               ("HTTP/1.1 200 " + "R" * 300
                                + "\r\nContent-Length: 0\r\n\r\n").encode("latin-1"), ep="small")))

    # The body cap, at the exact boundary, in each of the three framings
    p.check("Content-Length body at the cap of 1024",
            (lambda r: is_ok(r, 200) and r.get("bodylen") == 1024)(
                http_staged(cfg, mock, response("HTTP/1.1 200 OK", ["Content-Length: 1024"],
                                                b"B" * 1024), ep="small")))
    p.check("Content-Length body one over the cap",
            is_err(http_staged(cfg, mock, response("HTTP/1.1 200 OK", ["Content-Length: 1025"],
                                                   b"B" * 1025), ep="small")))
    p.check("single chunk at the cap of 1024",
            (lambda r: is_ok(r, 200) and r.get("bodylen") == 1024)(
                http_staged(cfg, mock, chunked(b"400\r\n" + b"B" * 0x400 + b"\r\n0\r\n\r\n"),
                            ep="small")))
    p.check("single chunk one over the cap",
            is_err(http_staged(cfg, mock, chunked(b"401\r\n" + b"B" * 0x401 + b"\r\n0\r\n\r\n"),
                               ep="small")))
    p.check("chunks cumulatively over the cap",
            is_err(http_staged(cfg, mock, chunked(b"320\r\n" + b"B" * 0x320 + b"\r\n320\r\n"
                                                  + b"B" * 0x320 + b"\r\n0\r\n\r\n"), ep="small")))
    p.check("close-delimited body at the cap of 1024",
            (lambda r: is_ok(r, 200) and r.get("bodylen") == 1024)(
                http_staged(cfg, mock, b"HTTP/1.1 200 OK\r\n\r\n" + b"B" * 1024,
                            keep=False, ep="small")))
    p.check("close-delimited body over the cap",
            is_err(http_staged(cfg, mock, b"HTTP/1.1 200 OK\r\n\r\n" + b"B" * 2000,
                               keep=False, ep="small")))
    p.check("trailer count over the cap",
            is_err(http_staged(cfg, mock,
                               chunked(b"2\r\nxy\r\n0\r\n"
                                       + b"".join(b"X-%d: v\r\n" % i for i in range(20)) + b"\r\n"),
                               ep="small")))

    # Content-Length exactly at the cap, then a truncated body: reserve() runs at the
    # cap, then the parser has to error on the short read rather than hang or read past
    p.check("Content-Length at the cap then a truncated body",
            is_err(http_staged(cfg, mock,
                               b"HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\n" + b"B" * 512,
                               keep=False, ep="small")))

    # The informational-response cap is not configurable: MAX_INFORMATIONAL_RESPONSES
    # in http.hpp is 8 for every endpoint
    p.check("1xx count at the cap of 8", is_ok(http_call(cfg, "/continue/8"), 200, "done"))
    p.check("1xx count one over the cap", is_err(http_call(cfg, "/continue/9")))
    p.check("1xx count far over the cap", is_err(http_call(cfg, "/continue/12")))

    # A 1xx that hides a body must not desync: the client ignores Content-Length on 1xx
    p.check("a 1xx hiding a body is refused rather than desyncing",
            is_err(http_staged(cfg, mock, b"HTTP/1.1 100 Continue\r\nContent-Length: 3\r\n\r\nXXX"
                                          b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")),
            security=True)

# PHASE: http_resource
def phase_http_resource(ctx):
    cfg, mock = ctx.cfg, ctx.http_mock
    p = ctx.phase("http_resource")

    # A slow upstream has to surface as a request timeout on the 5s-budget endpoint.
    # The mock stalls 20s, so the timeout always wins; the inbound read window is wide
    # enough to receive WFX's eventual answer
    t0 = time.time()
    p.check("slow headers surface as a request timeout",
            is_errc(http_call(cfg, "/slow-headers", ep="fast", rtimeout=18), EP_REQ_TIMEOUT),
            "elapsed %.1fs" % (time.time() - t0))
    p.check("slow body surfaces as a request timeout",
            is_errc(http_call(cfg, "/slow-body", ep="fast", rtimeout=18), EP_REQ_TIMEOUT))

    # Pool exhaustion on a two-slot pool: every request must resolve, none may succeed.
    # The overflow shows up as pool-exhausted or as a timeout, never as a 200
    results = in_parallel(lambda i: http_call(cfg, "/slow-headers", ep="fast", rtimeout=30), 4)
    p.check("a burst wider than the pool resolves without hanging",
            all(answered(r) for r in results) and all(not is_ok(r) for r in results),
            "results=%r" % results)

    # Coalescing: 16 concurrent identical GETs collapse to one backend call. All 16 also
    # succeed on a four-slot pool, since sharing one round trip means they need one slot
    # between them rather than one each
    mock.coalesce_reset()
    results = in_parallel(lambda i: http_call(cfg, "/coalesce", ep="coalesce"), 16)
    hits = mock.coalesce_hits()
    p.check("coalesce: all 16 waiters succeed",
            all(is_ok(r, 200, "coalesced") for r in results), "results=%r" % results)
    p.check("coalesce: the backend was called once", hits == 1, "backend hits=%d, expected 1" % hits)

    # Control: without a coalesce key the same requests are not deduplicated, so each
    # reaches the backend on its own slot. Driven at the pool's exact size, because past
    # that the surplus is refused rather than sent, and the count would then turn on how
    # fast slots recycle
    mock.coalesce_reset()
    in_parallel(lambda i: http_call(cfg, "/coalesce", ep="default"), DEFAULT_CONN_LIMIT)
    hits = mock.coalesce_hits()
    p.check("no coalesce key: one backend call each", hits == DEFAULT_CONN_LIMIT,
            "backend hits=%d, expected %d" % (hits, DEFAULT_CONN_LIMIT))

    # Clone integrity: every coalesced waiter gets its own full copy of the body
    mock.coalesce_reset()
    results = in_parallel(lambda i: http_call(cfg, "/coalesce-big", ep="coalesce"), 16)
    p.check("coalesce: every waiter got its own full 1000-byte body",
            all(is_ok(r, 200) and r.get("bodylen") == 1000 and r.get("body") == "C" * 1000
                for r in results),
            "a waiter got a truncated or aliased body")

    # Error fan-out: one failing backend call has to fail every coalesced waiter
    mock.coalesce_reset()
    results = in_parallel(lambda i: http_call(cfg, "/coalesce-bad", ep="coalesce"), 16)
    p.check("coalesce: a failing call fails every waiter", all(is_err(r) for r in results),
            "results=%r" % results)

    # Two distinct keys in flight at once: each dedupes to its own single backend call
    # AND every waiter receives ITS key's body, never the other group's. A key collision
    # or a cross-delivery here is an information-disclosure leak
    mock.coalesce_reset()

    def fire(i):
        if i % 2 == 0:
            return ("small", http_call(cfg, "/coalesce", ep="coalesce"))
        return ("big", http_call(cfg, "/coalesce-big", ep="coalesce"))

    results = in_parallel(fire, 16)
    hits = mock.coalesce_hits()
    small_ok = all(is_ok(r, 200, "coalesced") for tag, r in results if tag == "small")
    big_ok = all(is_ok(r, 200) and r.get("body") == "C" * 1000
                 for tag, r in results if tag == "big")
    p.check("coalesce: two keys in flight never cross-deliver", small_ok and big_ok,
            "results=%r" % results, security=True)
    p.check("coalesce: two keys mean exactly two backend calls", hits == 2,
            "backend hits=%d, expected 2" % hits)

# PHASE: http_fragmentation
def phase_http_fragmentation(ctx):
    cfg, mock = ctx.cfg, ctx.http_mock
    p = ctx.phase("http_fragmentation")

    def with_body(body):
        return ("HTTP/1.1 200 OK\r\nX-Mark: m\r\nContent-Length: %d\r\n\r\n%s"
                % (len(body), body)).encode("latin-1")

    # Whole valid responses delivered one byte at a time: the client has to reassemble
    # the status line, each header, the blank line and the body across a recv()
    # boundary at literally every byte
    p.check("drip: Content-Length body",
            is_ok(http_drip(cfg, mock, with_body("hello"), piece=1), 200, "hello"))
    p.check("drip: empty body", is_ok(http_drip(cfg, mock, with_body(""), piece=1), 200, ""))
    p.check("drip: two-byte pieces",
            is_ok(http_drip(cfg, mock, with_body("abcdefgh"), piece=2), 200, "abcdefgh"))
    p.check("drip: chunked body",
            is_ok(http_drip(cfg, mock, chunked(b"3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n"), piece=1),
                  200, "abcde"))
    p.check("drip: chunked with an extension and a trailer",
            is_ok(http_drip(cfg, mock, chunked(b"3;x=y\r\nabc\r\n0\r\nX-T: v\r\n\r\n"), piece=1),
                  200, "abc"))

    # A 1xx interleave delivered fragmented: flags from the 1xx block must not leak
    interleaved = b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone"
    p.check("drip: 1xx then the final response",
            is_ok(http_drip(cfg, mock, interleaved, piece=1), 200, "done"))
    p.check("split: 1xx then the final response",
            is_ok(http_split(cfg, mock, interleaved, 17), 200, "done"))

    # One split at EVERY byte offset of a fixed-length response, as one aggregate check
    blob = with_body("SPLITBODY")
    bad_offset = None
    for offset in range(1, len(blob)):
        if not is_ok(http_split(cfg, mock, blob, offset), 200, "SPLITBODY"):
            bad_offset = offset
            break
    p.check("split: a Content-Length response survives a split at every offset",
            bad_offset is None, "misframed when split at byte %r" % bad_offset)

    # Splits inside a chunk size line and inside chunk data
    body = chunked(b"5\r\nabcde\r\n3\r\nfgh\r\n0\r\n\r\n")
    header_len = len(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")
    for offset in (0, 1, 3, 5, 8, 12):
        p.check("split: chunked body at body offset %d" % offset,
                is_ok(http_split(cfg, mock, body, header_len + offset), 200, "abcdefgh"))

    # A larger body, coarse and fine, where the exact length has to survive
    big = with_body("Q" * 500)
    p.check("split: a 500-byte body split at its midpoint",
            (lambda r: is_ok(r, 200) and r.get("bodylen") == 500)(
                http_split(cfg, mock, big, len(big) // 2)))
    p.check("drip: a 500-byte body in eight-byte pieces",
            (lambda r: is_ok(r, 200) and r.get("bodylen") == 500)(
                http_drip(cfg, mock, big, piece=8)))

# PHASE: http_methods
def phase_http_methods(ctx):
    cfg = ctx.cfg
    p = ctx.phase("http_methods")

    for method in ("GET", "OPTIONS", "DELETE"):
        head = http_request_head(cfg, method=method)
        p.check("%s: request line is exact" % method,
                bool(head) and head.startswith("%s /reflectraw HTTP/1.1|" % method),
                "head=%r" % head)
        p.check("%s: no body means no Content-Length" % method,
                bool(head) and count_ci(head, "content-length") == 0, "head=%r" % head)

    for method in ("POST", "PUT", "PATCH"):
        head = http_request_head(cfg, method=method, x_body="abcd")
        p.check("%s: body carries a matching Content-Length" % method,
                bool(head) and head.startswith("%s /reflectraw HTTP/1.1|" % method)
                and "|Content-Length: 4|" in head, "head=%r" % head)

    # Every verb also has to round-trip against a live upstream
    for method in ("GET", "OPTIONS", "DELETE", "POST", "PUT", "PATCH"):
        r = http_call(cfg, "/ok", method=method)
        p.check("%s round-trips" % method, is_ok(r, 200, "hello"), "got %r" % r)

    p.check("HEAD stays bodyless even when the upstream advertises a Content-Length",
            is_ok(http_call(cfg, "/evil/headbody", method="HEAD"), 200, ""))

# PHASE: http_security
def phase_http_security(ctx):
    cfg, mock = ctx.cfg, ctx.http_mock
    p = ctx.phase("http_security")

    # No cross-request bleed across keep-alive reuse. Two distinct responses share one
    # connection and one parse state, so neither status, body nor header set may bleed
    # into the other, in either interleaving
    mock.stage("secA", b"HTTP/1.1 201 Created\r\nX-A: aaa\r\nContent-Length: 5\r\n\r\nAAAAA", True)
    mock.stage("secB", b"HTTP/1.1 202 Accepted\r\nX-B: bbb\r\nContent-Length: 3\r\n\r\nBBB", True)
    p.check("reuse: response A is intact", is_ok(http_call(cfg, "/raw/secA", ep="reuse"), 201, "AAAAA"))
    p.check("reuse: response B is intact", is_ok(http_call(cfg, "/raw/secB", ep="reuse"), 202, "BBB"))
    p.check("reuse: A then B then A shows no bleed",
            is_ok(http_call(cfg, "/raw/secA", ep="reuse"), 201, "AAAAA"), "", security=True)
    p.check("reuse: B never surfaces A's header",
            (lambda r: is_ok(r, 202) and (r.get("hdr") in (None, "")))(
                http_call(cfg, "/raw/secB", ep="reuse", want="X-A")),
            "B surfaced A's header", security=True)

    # Trailer headers must NEVER surface as response headers
    mock.stage("sectr", chunked(b"2\r\nhi\r\n0\r\nX-Trailer-Secret: leak\r\nSet-Cookie: e=1\r\n\r\n"),
               True)
    p.check("trailer: the body is still correct", is_ok(http_call(cfg, "/raw/sectr"), 200, "hi"))
    p.check("trailer: a trailer header never surfaces as a response header",
            (lambda r: is_ok(r, 200) and (r.get("hdr") in (None, "")))(
                http_call(cfg, "/raw/sectr", want="X-Trailer-Secret")),
            "a trailer surfaced as a header", security=True)
    p.check("trailer: a trailer Set-Cookie never surfaces either",
            (lambda r: is_ok(r, 200) and (r.get("hdr") in (None, "")))(
                http_call(cfg, "/raw/sectr", want="Set-Cookie")),
            "a trailer cookie surfaced", security=True)

    # A 1xx header block must not leak into the final response's headers
    mock.stage("secinfo", b"HTTP/1.1 103 Early Hints\r\nX-Info-Leak: secret\r\nLink: </s>\r\n\r\n"
                          b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", True)
    p.check("1xx: the final body is correct", is_ok(http_call(cfg, "/raw/secinfo"), 200, "hi"))
    p.check("1xx: a header from the 1xx block never leaks into the final response",
            (lambda r: is_ok(r, 200) and (r.get("hdr") in (None, "")))(
                http_call(cfg, "/raw/secinfo", want="X-Info-Leak")),
            "a 1xx header leaked into the final response", security=True)

    # Bytes past Content-Length are not part of the body
    r = http_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ngoodSMUGGLE!")
    p.check("the body stops at Content-Length and the extra bytes are dropped",
            is_ok(r, 200, "good") and r.get("bodylen") == 4,
            "the client delivered bytes past Content-Length: %r" % r, security=True)

    # Keep-alive poisoning through chunked and no-body-status framings
    for name, blob in (
        ("chunked trailing smuggle",
         chunked(b"4\r\ngood\r\n0\r\n\r\n")
         + b"HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nSMUGGLE"),
        ("204 with a chunked body",
         b"HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"),
        ("304 with a chunked body",
         b"HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"),
        ("204 with a hidden Content-Length body",
         b"HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\nhello"
         b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nBAD"),
    ):
        http_staged(cfg, mock, blob, ep="small")
        p.check("no pool poison after: %s" % name, pool_stays_clean(cfg, "small", n=12),
                "the pooled connection was poisoned by %s" % name, security=True)

    # A 204 whose illegal body is dribbled in LATE, after the client has already
    # completed the no-body response and returned the socket to the pool, cannot be
    # discarded at completion time, so the phantom bytes land on an idle keep-alive
    # connection. The engine releases a pooled slot that receives unsolicited bytes, but
    # that races with immediate reuse, so the very next request may break. What must
    # hold regardless of the race is that the phantom bytes are never delivered as a
    # body, and that the pool recovers. PHANTOM99 is not a valid response, so if reuse
    # loses the race the poisoned request errors rather than surfacing the payload
    http_drip(cfg, mock, b"HTTP/1.1 204 No Content\r\nContent-Length: 9\r\n\r\nPHANTOM99",
              piece=1, ep="small")
    results = [http_call(cfg, "/ok", ep="small") for _ in range(10)]
    p.check("a late 204 body is never smuggled out as a response",
            all((r is None) or r.get("body") != "PHANTOM99" for r in results),
            "the phantom 204 body surfaced as a response", security=True)
    p.check("the pool recovers after a late 204 body",
            any(is_ok(r, 200, "hello") for r in results[-4:]),
            "the pool never recovered after a late 204 body")

    # Injection breadth: every CR, LF and NUL form has to be refused
    for name, payload in (
        ("bare CR", b"/a\rb"), ("bare LF", b"/a\nb"), ("CRLF", b"/a\r\nb"),
        ("NUL", b"/a\x00b"), ("LF then CR", b"/a\n\rb"), ("CR CR LF", b"/a\r\r\nb"),
        ("smuggle line", b"/a\r\nHost: evil\r\nGET /x HTTP/1.1\r\n"),
        ("trailing CRLF", b"/a\r\n"), ("leading CRLF", b"\r\n/a"),
        ("double CRLF", b"/a\r\n\r\nGET /evil HTTP/1.1"),
    ):
        r = http_inject(cfg, "path", payload)
        p.check("reject path: %s" % name, is_errc(r, EP_SERIALIZE),
                "expected a serialize error, got %r" % r, security=True)

    for name, payload in (
        ("CR value", b"X-E: a\rb"), ("LF value", b"X-E: a\nb"), ("CRLF value", b"X-E: a\r\nb"),
        ("NUL value", b"X-E: a\x00b"), ("CR name", b"X-E\rX: b"), ("LF name", b"X-E\nX: b"),
        ("NUL name", b"X-\x00E: b"),
        ("smuggle value", b"X-E: a\r\nContent-Length: 0\r\n\r\nGET /evil HTTP/1.1"),
    ):
        r = http_inject(cfg, "header", payload)
        p.check("reject header: %s" % name, is_errc(r, EP_SERIALIZE),
                "expected a serialize error, got %r" % r, security=True)

    # Bytes that are not CR, LF or NUL are outside the filter and pass through. Driven
    # to document exactly where the line is drawn: none of them can smuggle anything,
    # so the client is expected to send them and WFX simply answers
    for name, payload in (("VT", b"/a\x0bb"), ("FF", b"/a\x0cb"), ("DEL", b"/a\x7fb"),
                          ("high 0xFF", b"/a\xffb"), ("tab", b"/a\tb")):
        p.check("path passthrough: %s" % name, answered(http_inject(cfg, "path", payload)))

    # DoS caps: unbounded input has to be refused rather than buffered forever
    p.check("a giant line with no newline trips the header cap",
            is_err(http_drip(cfg, mock, b"H" * 4096, piece=64, ep="small", keep=False)))
    flood = ("HTTP/1.1 200 OK\r\n" + "".join("X-%d: v\r\n" % i for i in range(500))
             + "Content-Length: 0\r\n\r\n").encode("latin-1")
    p.check("header-count amplification is capped",
            is_err(http_staged(cfg, mock, flood, ep="small")))

    # A pile of pipelined responses: only the first is delivered, none smuggled
    burst = b"".join(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nR%02d" % i for i in range(8))
    r = http_staged(cfg, mock, burst)
    p.check("only the first of a pipelined burst is delivered",
            is_ok(r, 200) and r.get("body") == "R00",
            "the client delivered a smuggled pipelined response: %r" % r, security=True)

# PHASE: http_lifecycle
def phase_http_lifecycle(ctx):
    cfg, mock = ctx.cfg, ctx.http_mock
    p = ctx.phase("http_lifecycle")

    # Connect refused: nothing is listening on that port. The request has to fail
    # cleanly inside the connect budget plus backoff, capped by the request budget,
    # never hang. The bound is generous because timeouts resolve on the 5s timer tick
    t0 = time.time()
    r = http_call(cfg, "/ok", ep="dead", rtimeout=18)
    elapsed = time.time() - t0
    p.check("a refused connection fails cleanly and promptly", is_err(r) and elapsed < 16,
            "elapsed %.1fs r=%r" % (elapsed, r))

    # Connect timeout: an unrouteable TEST-NET address, so the SYN is black-holed
    t0 = time.time()
    r = http_call(cfg, "/ok", ep="unreach", rtimeout=24)
    elapsed = time.time() - t0
    p.check("an unreachable host fails inside the connect budget", is_err(r) and elapsed < 22,
            "elapsed %.1fs r=%r" % (elapsed, r))

    p.check("worker survives both connect failures", wfx_healthy(cfg) and mock.ping())

    # Reconnect: request one rides a connection the server closes, request two has to
    # reconnect transparently
    p.check("a closed connection is transparently replaced",
            is_ok(http_call(cfg, "/close", ep="reuse"), 200, "closebody")
            and is_ok(http_call(cfg, "/ok", ep="reuse"), 200, "hello"))

    # Keep-alive reuse: two back-to-back requests ride the same pooled connection, so
    # the mock's per-connection request counter climbs from 1 to 2
    first = http_call(cfg, "/kacount", ep="idle")
    second = http_call(cfg, "/kacount", ep="idle")
    p.check("keep-alive reuses the pooled connection",
            is_ok(first, 200, "1") and is_ok(second, 200, "2"),
            "first=%r second=%r" % (first, second))

    # Idle timeout: after idleTimeoutSeconds (5s, noticed on the next 5s tick) the
    # pooled connection is closed, so the next request opens a fresh one and the
    # per-connection counter starts over
    time.sleep(12)
    r = http_call(cfg, "/kacount", ep="idle")
    p.check("an idle connection is recycled rather than reused", is_ok(r, 200, "1"),
            "expected a fresh connection (body '1'), got %r" % r)

    # Prewarm: EpPrewarm opened its connections eagerly at boot, before any request was
    # driven anywhere. The mock accepted them but they sent nothing, so the boot-time
    # snapshot of accepted-minus-served was at least prewarm (3)
    p.check("prewarm opened its connections at boot", cfg.prewarm_idle >= 3,
            "idle prewarmed connections at boot=%d, expected at least 3" % cfg.prewarm_idle)

# PHASE: http_metrics
def phase_http_metrics(ctx):
    cfg, mock = ctx.cfg, ctx.http_mock
    p = ctx.phase("http_metrics")

    start = metrics(cfg)
    p.check("metrics: latency histograms are enabled", start.get("latency_enabled") is True,
            "expected [Metrics] latency = true in wfx.local.toml, got %r"
            % start.get("latency_enabled"))
    p.check("metrics: every endpoint is registered with its host identity",
            len(start.get("endpoints", [])) > 0 and all("host" in e for e in start["endpoints"]),
            "endpoints=%r" % start.get("endpoints"))
    in_use = metric(start, "slots_in_use")
    p.check("metrics: the in-use slot gauge is quiescent at rest", in_use == 0,
            "expected 0 in-use slots, got %d" % in_use)

    # Success path: requests, completions, the 2xx bucket and latency samples each move
    # by exactly the number driven, and bytes flow in both directions
    driven = 20
    results = [http_call(cfg, "/ok") for _ in range(driven)]
    p.check("metrics: the driven calls all succeeded",
            all(is_ok(r, 200, "hello") for r in results), "results=%r" % results)

    after = metrics(cfg)
    for field in ("requests", "completed", "status_2xx"):
        moved = metric(after, field) - metric(start, field)
        p.check("metrics: %s matches the calls driven" % field, moved == driven,
                "expected +%d, got +%d" % (driven, moved))

    moved = latency_metric(after, "count") - latency_metric(start, "count")
    p.check("metrics: one latency sample per completion", moved == driven,
            "expected +%d, got +%d" % (driven, moved))
    p.check("metrics: bytes_out is recorded on send",
            metric(after, "bytes_out") - metric(start, "bytes_out") > 0)
    p.check("metrics: bytes_in is recorded on receive",
            metric(after, "bytes_in") - metric(start, "bytes_in") > 0)

    # Each non-2xx status class lands in its own bucket, and in no other
    for code, field, other in ((301, "status_3xx", "status_2xx"),
                               (404, "status_4xx", "status_2xx"),
                               (503, "status_5xx", "status_4xx")):
        before = metrics(cfg)
        count = 4
        results = [http_call(cfg, "/status/%d" % code) for _ in range(count)]
        after = metrics(cfg)
        p.check("metrics: the %d calls completed" % code, all(is_ok(r, code) for r in results),
                "results=%r" % results)
        p.check("metrics: %s matches the %d calls driven" % (field, code),
                metric(after, field) - metric(before, field) == count,
                "expected +%d, got +%d" % (count, metric(after, field) - metric(before, field)))
        p.check("metrics: %d never lands in %s" % (code, other),
                metric(after, other) - metric(before, other) == 0)

    p.check("metrics: the 1xx bucket stays zero, no final status is ever 1xx",
            metric(after, "status_1xx") - metric(start, "status_1xx") == 0)

    # Connect failure: the attempt is counted as a request and classed as a connect
    # failure, and never as a completion. This is the synchronous ECONNREFUSED path
    before = metrics(cfg)
    count = 2
    results = [http_call(cfg, "/ok", ep="dead", rtimeout=18) for _ in range(count)]
    after = metrics(cfg)
    p.check("metrics: the dead-endpoint calls failed cleanly", all(is_err(r) for r in results),
            "results=%r" % results)
    p.check("metrics: connect failures are counted",
            metric(after, "connect_failures") - metric(before, "connect_failures") == count,
            "expected +%d, got +%d"
            % (count, metric(after, "connect_failures") - metric(before, "connect_failures")))
    p.check("metrics: a failed attempt is still counted as a request",
            metric(after, "requests") - metric(before, "requests") == count,
            "expected +%d, got +%d"
            % (count, metric(after, "requests") - metric(before, "requests")))
    p.check("metrics: a failed call is never counted as completed",
            metric(after, "completed") - metric(before, "completed") == 0)

    # Request timeout: a backend that stalls past the fast endpoint's 5s budget
    before = metrics(cfg)
    r = http_call(cfg, "/slow-headers", ep="fast", rtimeout=18)
    after = metrics(cfg)
    p.check("metrics: the slow backend timed out", is_errc(r, EP_REQ_TIMEOUT), "r=%r" % r)
    p.check("metrics: the request timeout is counted",
            metric(after, "request_timeouts") - metric(before, "request_timeouts") == 1,
            "expected +1, got +%d"
            % (metric(after, "request_timeouts") - metric(before, "request_timeouts")))

    # Pool exhaustion: HttpEndpointConfig allocates connLimit exactly, so the fast
    # endpoint really does hold two slots. A burst wider than that is refused outright
    # with POOL_EXHAUSTED, one metric increment per refusal, never a completion
    before = metrics(cfg)
    burst = in_parallel(lambda i: http_call(cfg, "/slow-headers", ep="fast", rtimeout=30), 16)
    after = metrics(cfg)
    refused = sum(1 for r in burst if is_errc(r, EP_POOL_EXHAUSTED))
    p.check("metrics: a burst wider than the pool refuses the surplus", refused >= 1,
            "not one of the 16 requests came back POOL_EXHAUSTED")
    p.check("metrics: pool exhaustion is counted once per refusal",
            metric(after, "pool_exhausted") - metric(before, "pool_exhausted") == refused,
            "refused=%d, pool_exhausted moved by %d"
            % (refused, metric(after, "pool_exhausted") - metric(before, "pool_exhausted")))
    p.check("metrics: a refused request is never also served",
            all(not is_ok(r) for r in burst), "a request in the burst came back 2xx")

    # A reply that is not valid HTTP has to be counted as a protocol error rather
    # than failing the call and leaving every bucket untouched
    before = metrics(cfg)
    r = http_staged(cfg, mock, b"NOT-HTTP AT ALL\r\n\r\n", keep=False)
    after = metrics(cfg)
    p.check("metrics: a malformed reply failed cleanly", is_err(r), "r=%r" % r)
    p.check("metrics: a parse failure is counted as another error",
            metric(after, "other_errors") - metric(before, "other_errors") >= 1,
            "expected at least +1, got +%d"
            % (metric(after, "other_errors") - metric(before, "other_errors")))

    # Coalescing: merged waiters are counted as coalesce hits, not as completions
    mock.coalesce_reset()
    before = metrics(cfg)
    results = in_parallel(lambda i: http_call(cfg, "/coalesce", ep="coalesce"), 16)
    after = metrics(cfg)
    p.check("metrics: the coalesced waiters all succeeded",
            all(is_ok(r, 200, "coalesced") for r in results), "results=%r" % results)
    p.check("metrics: coalesce hits are recorded",
            metric(after, "coalesce_hits") - metric(before, "coalesce_hits") > 0,
            "expected more than 0, got +%d"
            % (metric(after, "coalesce_hits") - metric(before, "coalesce_hits")))

    # Gauge invariant: every lease taken above has since been returned, which is what
    # catches an unbalanced increment and decrement
    end = metrics(cfg)
    p.check("metrics: the in-use slot gauge is back to zero after the phase",
            metric(end, "slots_in_use") == 0,
            "expected 0 in-use slots, got %d" % metric(end, "slots_in_use"))

    p.check("worker healthy after the metrics phase", wfx_healthy(cfg) and mock.ping())

# PHASE: smtp_handshake
def phase_smtp_handshake(ctx):
    cfg, mock = ctx.cfg, ctx.smtp_mock
    p = ctx.phase("smtp_handshake")

    r = smtp_send(cfg, "good", body=b"hello world")
    p.check("good: the full transaction round-trips", smtp_ok(r), "r=%r" % r)
    p.check("good: the final SMTP code is 250", bool(r) and r.get("code") == 250, "r=%r" % r)

    stats = mock.stats("good")
    p.check("good: the mock recorded a completed TLS handshake", stats.get("handshakes", 0) >= 1,
            "stats=%r" % stats)
    p.check("good: the mock recorded a successful AUTH", stats.get("auth_ok", 0) >= 1,
            "stats=%r" % stats)

    # SendMail returns a WFX::Coro the route co_awaits, so this is what proves one
    # coroutine can be awaited from another end to end, not merely that it compiles
    r = smtp_send_mail(cfg, "good", body=b"through SendMail")
    p.check("good: SendMail drives the whole transaction in one call",
            bool(r) and r.get("ep") == EP_SUCCESS and r.get("success") is True, "r=%r" % r)

    # RFC 5321 4.5.2 dot-stuffing: a body line starting with '.' has to survive the
    # client's stuffing and the mock's un-stuffing byte for byte, rather than being
    # swallowed as though it were the DATA terminator
    mock.reset("good")
    r = smtp_send(cfg, "good", body=b"Hello\n.leading dot line\nEnd")
    p.check("good: a dot-stuffed body round-trips", smtp_ok(r), "r=%r" % r)

    bodies = mock.stats("good").get("bodies", [])
    p.check("good: the mock received exactly one DATA body", len(bodies) == 1, "bodies=%r" % bodies)
    p.check("good: the leading-dot line survived, not swallowed as the terminator",
            "\r\n.leading dot line\r\n" in (bodies[-1] if bodies else ""),
            "body=%r" % (bodies[-1] if bodies else None))

# PHASE: smtp_auth
def phase_smtp_auth(ctx):
    cfg, mock = ctx.cfg, ctx.smtp_mock
    p = ctx.phase("smtp_auth")

    r = smtp_send(cfg, "auth_login_only")
    p.check("auth_login_only: AUTH LOGIN is used when PLAIN is not offered", smtp_ok(r), "r=%r" % r)
    stats = mock.stats("auth_login_only")
    p.check("auth_login_only: the mock recorded the AUTH LOGIN success",
            stats.get("auth_ok", 0) >= 1, "stats=%r" % stats)

    r = smtp_send(cfg, "auth_fail")
    p.check("auth_fail: wrong credentials are refused", is_errc(r, EP_INTERNAL), "r=%r" % r)
    p.check("auth_fail: refused at the mail stage, so the handshake never completed",
            bool(r) and r.get("stage") == "mail", "r=%r" % r)
    stats = mock.stats("auth_fail")
    p.check("auth_fail: the mock recorded the failed AUTH", stats.get("auth_fail", 0) >= 1,
            "stats=%r" % stats)
    p.check("auth_fail: the client never treated itself as authenticated",
            stats.get("auth_ok", 0) == 0, "stats=%r" % stats)

    r = smtp_send(cfg, "no_auth_mechs")
    p.check("no_auth_mechs: refused rather than falling back to an unimplemented mechanism",
            is_errc(r, EP_INTERNAL), "r=%r" % r)

# PHASE: smtp_starttls
def phase_smtp_starttls(ctx):
    cfg, mock = ctx.cfg, ctx.smtp_mock
    p = ctx.phase("smtp_starttls")

    r = smtp_send(cfg, "no_starttls")
    p.check("no_starttls: the client never falls back to plaintext AUTH",
            is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)
    stats = mock.stats("no_starttls")
    p.check("no_starttls: the mock never even saw a TLS handshake attempt",
            stats.get("handshakes", 0) == 0, "stats=%r" % stats)

    # CVE-2011-0411 / CVE-2026-41319 class: plaintext spliced in right after the
    # STARTTLS go-ahead. A correct client discards what is already buffered before the
    # TLS wrap, so anything still unread corrupts the handshake's own read and fails
    # closed. The bar is that the injected bytes are never trusted, not that the
    # handshake survives them
    mock.reset("inject")
    r = smtp_send(cfg, "inject", body=b"still fine")
    p.check("inject: fails closed rather than falling back to plaintext",
            is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)
    stats = mock.stats("inject")
    p.check("inject: the pre-TLS plaintext never reaches an authenticated session",
            stats.get("auth_ok", 0) == 0 and not stats.get("bodies"), "stats=%r" % stats,
            security=True)

    r = smtp_send(cfg, "mismatched_code")
    p.check("mismatched_code: a spliced continuation line is refused, not silently accepted",
            is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)

    r = smtp_send(cfg, "malformed_greeting")
    p.check("malformed_greeting: a non-SMTP banner is refused", is_errc(r, EP_INTERNAL), "r=%r" % r)

# PHASE: smtp_certs
def phase_smtp_certs(ctx):
    cfg, mock = ctx.cfg, ctx.smtp_mock
    p = ctx.phase("smtp_certs")

    labels = {
        "selfsigned": "an untrusted self-signed STARTTLS cert is refused",
        "wronghost":  "a hostname-mismatched STARTTLS cert is refused",
        "expired":    "an expired STARTTLS cert is refused",
    }
    for name, port, cert, opts, expect in SMTP_PERSONAS:
        if name not in labels:
            continue
        if not smtp_cert_available(cfg, cert):
            p.check(labels[name] + " (skipped, cert unavailable)", True)
            continue

        r = smtp_send(cfg, name)
        p.check(labels[name], is_err(r), "the client ACCEPTED it (r=%r), a MitM is possible" % r,
                security=True)

        stats = mock.stats(name)
        p.check("%s: the client bailed at the TLS layer" % name, stats.get("hs_fail", 0) >= 1,
                "stats=%r, expected at least one failed handshake" % stats, security=True)

# PHASE: smtp_resource
def phase_smtp_resource(ctx):
    cfg = ctx.cfg
    p = ctx.phase("smtp_resource")

    # Connect-phase hangs: a response flood, oversized unterminated lines, or the whole
    # handshake trickled a byte at a time. Two legitimate outcomes, both proving the
    # client never actually hangs: LineResponse's own maxResponseLines and
    # maxResponseLineBytes caps reject a malformed or oversized line fast, which is
    # faster than any timeout, or connectTimeoutSeconds ends it. Which one fires is a
    # race between two independent defenses, not something worth pinning to one code
    for name in ("flood_greeting", "flood_ehlo2", "huge_line_greeting", "huge_line_ehlo2",
                 "slow_trickle"):
        r = smtp_send(cfg, name, rtimeout=15.0)
        p.check("%s: the hostile handshake is refused rather than hanging forever" % name,
                is_errc(r, EP_INTERNAL) or is_errc(r, EP_HANDSHAKE_TIMEOUT), "r=%r" % r)

    # Post-connect hang: the mock authenticates normally, then goes silent forever the
    # moment DATA arrives. requestTimeoutSeconds, not connectTimeoutSeconds, is what has
    # to catch this
    r = smtp_send(cfg, "silent_data", rtimeout=15.0)
    p.check("silent_data: a mid-transaction hang times out on the request budget",
            is_errc(r, EP_REQ_TIMEOUT), "r=%r" % r)
    p.check("silent_data: the timeout is reported at the data_start stage",
            bool(r) and r.get("stage") == "data_start", "r=%r" % r)

# PHASE: smtp_drops
def phase_smtp_drops(ctx):
    cfg = ctx.cfg
    p = ctx.phase("smtp_drops")

    for name in ("drop_greeting", "drop_pre_handshake", "drop_starttls", "drop_auth"):
        r = smtp_send(cfg, name)
        p.check("%s: an abrupt close during the handshake is refused cleanly" % name,
                is_errc(r, EP_INTERNAL), "r=%r" % r)
        p.check("%s: the refusal is reported at the mail stage" % name,
                bool(r) and r.get("stage") == "mail", "r=%r" % r)

    # A drop after DATA's 354 go-ahead but before the body: the handshake and both
    # MAIL and RCPT already succeeded, so this is a live-connection drop rather than a
    # connect failure
    r = smtp_send(cfg, "drop_data_prompt")
    p.check("drop_data_prompt: an abrupt close after the DATA prompt is refused, not hung",
            is_err(r), "r=%r" % r)

# PHASE: smtp_inject
def phase_smtp_inject(ctx):
    cfg = ctx.cfg
    p = ctx.phase("smtp_inject")

    vectors = {
        "mailfrom": b"attacker@evil.com>\r\nRCPT TO:<victim@x>\r\nDATA\r\nsmuggled\r\n.\r\nMAIL FROM:<x",
        "rcptto":   b"victim@x>\r\nDATA\r\nsmuggled\r\n.\r\nMAIL FROM:<x",
        "fromname": b"Evil\r\nBcc: attacker@evil.com",
        "toname":   b"Evil\r\nBcc: attacker@evil.com",
        "subject":  b"innocuous\r\nBcc: attacker@evil.com",
        "replyto":  b"attacker@evil.com\r\nBcc: attacker@evil.com",
        # NUL only: CR and LF are legitimate body line structure
        "body":     b"line one\x00line two",
    }
    for field, payload in vectors.items():
        r = smtp_inject(cfg, field, payload)
        p.check("%s: CR/LF/NUL injection is refused at the serializer" % field,
                is_errc(r, EP_SERIALIZE), "r=%r" % r, security=True)

    # heloName is operator config today rather than per-request input, but it is
    # screened anyway (see smtp.hpp's SmtpOnConnect): CPython's smtplib had a real CVE
    # through this exact parameter, local_hostname, bpo-30585. A poisoned heloName must
    # never even open the connection
    r = smtp_send(cfg, "heloinject", rtimeout=10.0)
    p.check("heloName: a CRLF-poisoned EHLO identity is refused before a byte reaches the wire",
            is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)

# PHASE: pg_framing
def phase_pg_framing(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_framing")

    # Full good-path baseline: pooled query, pinned session, chunked stream. Nothing
    # past this point means anything if the happy path itself does not work
    r = pg_query(cfg)
    p.check("good: a pooled query succeeds", is_ok(r) and not r.get("failed"), "r=%r" % r)
    p.check("good: the default row decodes as expected", r.get("value") == "1", "r=%r" % r)

    r = pg_session(cfg, statements=["SELECT 1"])
    p.check("good: a pinned session begins, runs one statement and commits",
            is_ok(r) and r.get("begin_ep") == EP_SUCCESS and r.get("finish_ep") == EP_SUCCESS, "r=%r" % r)

    r = pg_stream(cfg)
    p.check("good: a chunked stream completes", is_ok(r) and not r.get("failed") and r.get("rows") == 5,
            "r=%r" % r)

    # Wire framing: an unrecognized message type, and a declared length over the
    # connection's own maxMessageBytes
    r = pg_query(cfg, persona="unknown_type_startup")
    p.check("unknown_type_startup: an unrecognized message type during the handshake is refused",
            is_errc(r, EP_INTERNAL), "r=%r" % r)

    r = pg_query(cfg, persona="huge_ready", rtimeout=15.0)
    p.check("huge_ready: a NoticeResponse over maxMessageBytes is refused, not buffered",
            is_errc(r, EP_INTERNAL), "r=%r" % r)

# PHASE: pg_ssl
def phase_pg_ssl(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_ssl")

    r = pg_query(cfg, persona="ssl_garbage")
    p.check("ssl_garbage: a verdict byte that is neither S nor N is refused",
            is_errc(r, EP_INTERNAL), "r=%r" % r)

    # CVE-2021-23214 class: plaintext spliced in right after the verdict byte, before
    # the TLS handshake even starts. A correct client never trusts bytes that arrive
    # before its own ClientHello could possibly have been answered
    r = pg_query(cfg, persona="ssl_inject")
    p.check("ssl_inject: a verdict trailed by extra plaintext is refused, not parsed as TLS",
            is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)

    r = pg_query(cfg, persona="ssl_reject_required")
    p.check("ssl_reject_required: a plaintext refusal is honored, PgEncryption::REQUIRED never falls back",
            is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)

# PHASE: pg_handshake
def phase_pg_handshake(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_handshake")

    for persona in ("drop_startup", "drop_auth_challenge", "drop_auth_final",
                    "drop_backendkeydata", "drop_ready"):
        r = pg_query(cfg, persona=persona)
        p.check("%s: an abrupt close during the handshake is refused cleanly" % persona,
                is_errc(r, EP_INTERNAL), "r=%r" % r)

    # CVE-2024-10977 class: a full ErrorResponse mid-handshake, from a peer not yet
    # authenticated, carrying attacker-shaped content (ANSI escapes, CR/LF). Any
    # BE_ERROR_RESPONSE before ReadyForQuery is unconditionally fatal (connection.hpp),
    # so this has to fail the same way a plain close does, not surface that content
    r = pg_query(cfg, persona="error_at_handshake")
    p.check("error_at_handshake: an ErrorResponse from an unauthenticated peer is refused, "
            "its content never trusted",
            is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)

# PHASE: pg_auth
def phase_pg_auth(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_auth")

    for persona in ("wrong_auth_md5", "wrong_auth_gss", "wrong_auth_sspi"):
        r = pg_query(cfg, persona=persona)
        p.check("%s: an unsupported auth method is refused, not silently skipped" % persona,
                is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)

    r = pg_query(cfg, persona="wrong_auth_cleartext")
    p.check("wrong_auth_cleartext: cleartext is refused under the default NO_PLAINTEXT policy",
            is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)

    r = pg_query(cfg, persona="cleartext_allowed")
    p.check("cleartext_allowed: cleartext succeeds only once the policy explicitly allows it",
            is_ok(r) and not r.get("failed"), "r=%r" % r)

# PHASE: pg_scram
def phase_pg_scram(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_scram")

    for persona in ("scram_offer_empty", "scram_offer_plus_only", "scram_offer_garbage"):
        r = pg_query(cfg, persona=persona)
        p.check("%s: a mechanism list without plain SCRAM-SHA-256 is refused" % persona,
                is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)

    for persona, what in (
        ("scram_bad_nonce",     "a server nonce that does not extend the client's own"),
        ("scram_iter_zero",     "a zero iteration count"),
        ("scram_iter_over",     "an iteration count past the DoS cap"),
        ("scram_bad_salt",      "a salt that decodes to nothing usable"),
        ("scram_bad_signature", "a forged server signature"),
    ):
        r = pg_query(cfg, persona=persona)
        p.check("%s: %s is rejected" % (persona, what), is_errc(r, EP_INTERNAL), "r=%r" % r,
                security=True)

    # The common real-world shape: a server that supports channel binding offers both
    # SCRAM-SHA-256-PLUS and plain SCRAM-SHA-256. WFX has no channel-binding support,
    # so refusing the whole exchange here would be a regression, not a defense
    r = pg_query(cfg, persona="scram_mixed")
    p.check("scram_mixed: a mechanism list offering PLUS alongside plain SCRAM-SHA-256 still "
            "authenticates on plain",
            is_ok(r) and not r.get("failed"), "r=%r" % r)

    # RFC 5802's e=<reason> server-final path (auth.hpp's ServerFinalIsError), never
    # exercised before this: the server rejects the exchange for its own reasons, not
    # a bad proof. VerifyServerFinal has no v= to check and fails the same way a
    # forged signature would, so this still has to fail closed
    r = pg_query(cfg, persona="scram_server_error")
    p.check("scram_server_error: an e=<reason> server-final is rejected the same as a missing "
            "or forged signature",
            is_errc(r, EP_INTERNAL), "r=%r" % r)

# PHASE: pg_wire
def phase_pg_wire(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_wire")

    r = pg_query(cfg, sql="SELECT PGAUDIT_UNKNOWN_TYPE")
    p.check("PGAUDIT_UNKNOWN_TYPE: an unrecognized message type mid-query is refused",
            is_errc(r, EP_INTERNAL), "r=%r" % r)

    r = pg_query(cfg, sql="SELECT PGAUDIT_DATAROW_FIRST")
    p.check("PGAUDIT_DATAROW_FIRST: a DataRow with no RowDescription this round is refused",
            is_errc(r, EP_INTERNAL), "r=%r" % r)

    # Protocol-confusion class: a message that only ever belongs to a feature this
    # client has no code path for, arriving where a query reply was expected.
    # wire.hpp's Parse has no case for any of these mid-query, only default: ERROR
    for marker, what in (
        ("PGAUDIT_COPY_UNSOLICITED",      "an unrequested CopyOutResponse (no COPY support)"),
        ("PGAUDIT_NEGOTIATE_PROTO",       "an unrequested protocol version negotiation"),
        ("PGAUDIT_NOTIFY_UNSOLICITED",    "an unrequested NotificationResponse (no LISTEN/NOTIFY support)"),
        ("PGAUDIT_BACKENDKEY_MIDQUERY",   "a resent BackendKeyData outside the handshake"),
    ):
        r = pg_query(cfg, sql="SELECT %s" % marker)
        p.check("%s: %s is refused" % (marker, what), is_errc(r, EP_INTERNAL), "r=%r" % r)

    # A non-streaming query getting PortalSuspended instead of CommandComplete.
    # streamRows == 0 on this request, so wire.hpp's case is a no-op: the row still
    # has to come through and the request still has to complete, not hang
    r = pg_query(cfg, sql="SELECT PGAUDIT_PORTAL_SUSPENDED_NOSTREAM")
    p.check("PGAUDIT_PORTAL_SUSPENDED_NOSTREAM: an out-of-context PortalSuspended is a no-op, "
            "not a hang",
            is_ok(r) and not r.get("failed") and r.get("rows") == 1, "r=%r" % r)

# PHASE: pg_results
def phase_pg_results(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_results")

    r = pg_query(cfg, sql="SELECT PGAUDIT_COLUMN_MISMATCH")
    p.check("PGAUDIT_COLUMN_MISMATCH: a DataRow claiming more fields than RowDescription is refused",
            is_errc(r, EP_INTERNAL), "r=%r" % r)

    r = pg_query(cfg, sql="SELECT PGAUDIT_ROWDESC_NEG")
    p.check("PGAUDIT_ROWDESC_NEG: a negative RowDescription column count is refused",
            is_errc(r, EP_INTERNAL), "r=%r" % r)

    # CVE-2024-10977 class at the query level: the server is authenticated by this
    # point, but its error text is still attacker-shaped content this suite controls
    # by construction. It has to survive PgError -> this route's JSON reflection
    # without corrupting the response (proven by _json parsing it at all) or losing
    # the sqlstate the caller actually needs
    r = pg_query(cfg, sql="SELECT PGAUDIT_ERROR_CONTROLCHARS")
    p.check("PGAUDIT_ERROR_CONTROLCHARS: a message with escape codes and embedded CR/LF/NUL "
            "still reflects as well-formed JSON with its sqlstate intact",
            is_ok(r) and r.get("failed") and r.get("sqlstate") == "XX000", "r=%r" % r)

    # CommandComplete's trailing digit run past uint64 range. SetCommandTag
    # (result.hpp) hands it to DecodeText<uint64_t>, which uses std::from_chars and
    # leaves the output at its default on out-of-range rather than wrapping
    r = pg_query(cfg, sql="SELECT PGAUDIT_HUGE_TAG")
    p.check("PGAUDIT_HUGE_TAG: an out-of-range affected-rows count decodes to 0, not a wrapped "
            "or garbage value",
            is_ok(r) and not r.get("failed") and r.get("rows") == 1 and r.get("affected_rows") == 0,
            "r=%r" % r)

# PHASE: pg_types
def phase_pg_types(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_types")

    r = pg_query(cfg, sql="SELECT PGAUDIT_NUMERIC_EXTREME")
    p.check("PGAUDIT_NUMERIC_EXTREME: a weight/ndigits pair past NUMERIC_POW10K_MAX decodes to "
            "infinity rather than reading past the digit array",
            is_ok(r) and not r.get("failed") and r.get("value") == "inf", "r=%r" % r)

    # PgArrayView::Count() (types.hpp) has no overflow guard on the dimension product, so
    # this documents the current behaviour rather than asserting the result is meaningful.
    # array_walked stays 0 regardless: the header claims 6 dimensions of 50000, but no
    # element bytes ever followed it, so Count()'s garbage product and what NextElement
    # can actually step are two independent, and here disagreeing, signals
    r = pg_query(cfg, sql="SELECT PGAUDIT_ARRAY_OVERFLOW")
    p.check("PGAUDIT_ARRAY_OVERFLOW: 6 dimensions is within MAX_ARRAY_DIMS, so DecodeArray accepts "
            "it and the worker does not crash computing Count()",
            is_ok(r) and not r.get("failed") and r.get("array_ok") is True and r.get("array_ndim") == 6,
            "r=%r" % r)
    p.check("PGAUDIT_ARRAY_OVERFLOW: Count()'s overflowed product has nothing walkable behind it",
            r.get("array_walked") == 0, "r=%r" % r)

    r = pg_query(cfg, sql="SELECT PGAUDIT_ARRAY_BADDIM")
    p.check("PGAUDIT_ARRAY_BADDIM: more dimensions than MAX_ARRAY_DIMS is rejected by DecodeArray",
            is_ok(r) and not r.get("failed") and r.get("array_ok") is False, "r=%r" % r)

    r = pg_query(cfg, sql="SELECT PGAUDIT_ARRAY_NEGDIM")
    p.check("PGAUDIT_ARRAY_NEGDIM: a negative dimension length is rejected by DecodeArray",
            is_ok(r) and not r.get("failed") and r.get("array_ok") is False, "r=%r" % r)

    # The good path none of the vectors above exercise: a well-formed array with real
    # element bytes actually has to decode through Get<PgArrayView> and NextElement,
    # not just survive a hostile header
    r = pg_query(cfg, sql="SELECT PGAUDIT_ARRAY_ELEMENTS")
    p.check("PGAUDIT_ARRAY_ELEMENTS: a well-formed int4[3] with a NULL element walks to "
            "exactly 3 elements, one of them NULL",
            is_ok(r) and not r.get("failed") and r.get("array_ok") is True and
            r.get("array_count") == 3 and r.get("array_walked") == 3 and r.get("array_any_null") is True,
            "r=%r" % r)

    # A declared element length longer than the bytes actually behind it. NextElement
    # has to stop the walk, not read past the end of the array's own byte range
    r = pg_query(cfg, sql="SELECT PGAUDIT_ARRAY_TRUNCATED")
    p.check("PGAUDIT_ARRAY_TRUNCATED: an element claiming more bytes than remain stops the "
            "walk instead of reading past the end",
            is_ok(r) and not r.get("failed") and r.get("array_ok") is True and
            r.get("array_count") == 1 and r.get("array_walked") == 0,
            "r=%r" % r)

# PHASE: pg_cache
def phase_pg_cache(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_cache")

    for persona, sqlstate in (("cache_epoch_feature", "0A000"), ("cache_epoch_badname", "26000")):
        r = pg_session(cfg, persona=persona, statements=["SELECT 1", "SELECT 1", "SELECT 1"],
                       isolation="none", finish="none")
        steps = r.get("steps") or []
        p.check("%s: query 1 succeeds before the fault" % persona,
                len(steps) == 3 and steps[0].get("ep") == EP_SUCCESS and not steps[0].get("failed"),
                "r=%r" % r)
        p.check("%s: query 2 carries the %s ErrorResponse" % (persona, sqlstate),
                len(steps) == 3 and steps[1].get("ep") == EP_SUCCESS and steps[1].get("failed") and
                steps[1].get("sqlstate") == sqlstate, "r=%r" % r)
        p.check("%s: query 3 succeeds again, the statement cache recovered from the invalidation" %
                persona,
                len(steps) == 3 and steps[2].get("ep") == EP_SUCCESS and not steps[2].get("failed"),
                "r=%r" % r)

    # Churn: more distinct SQL than PgCfg's statementCacheSize (8) has slots for,
    # forcing eviction under STMT_PROBE_LEN (stmt_cache.hpp). Nothing here needs a
    # name to survive; the only claim is that eviction never corrupts an unrelated,
    # later query
    r = pg_session(cfg, statements=["SELECT %d AS n" % i for i in range(12)],
                   isolation="none", finish="none")
    steps = r.get("steps") or []
    p.check("statement cache churn: 12 distinct statements against an 8-slot cache all succeed",
            len(steps) == 12 and all(s.get("ep") == EP_SUCCESS and not s.get("failed") for s in steps),
            "r=%r" % r)

# PHASE: pg_stream
def phase_pg_stream(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_stream")

    r = pg_stream(cfg, chunk_rows=2)
    p.check("wire_stream: a 5-row result reads in chunks of 2",
            is_ok(r) and not r.get("failed") and r.get("rows") == 5, "r=%r" % r)
    p.check("wire_stream: three rounds cover 5 rows two at a time", r.get("chunks") == 3, "r=%r" % r)

    r = pg_stream(cfg, chunk_rows=1)
    p.check("wire_stream: chunk_rows=1 reads every row as its own PortalSuspended round",
            is_ok(r) and not r.get("failed") and r.get("rows") == 5 and r.get("chunks") == 5, "r=%r" % r)

    # A third identical query on this connection: statementCacheMinUses (2) has been
    # reached, so this round names the statement instead of re-Parsing. Streaming has
    # to keep working from a Bind-only opening round, not just a fresh Parse one
    r = pg_stream(cfg, chunk_rows=100)
    p.check("wire_stream: a named (statement-cached) stream still finishes in one round",
            is_ok(r) and not r.get("failed") and r.get("rows") == 5 and r.get("chunks") == 1, "r=%r" % r)

# PHASE: pg_cancel
def phase_pg_cancel(ctx):
    cfg, mock = ctx.cfg, ctx.pg_mock
    p = ctx.phase("pg_cancel")

    mock.reset("cancel_probe")
    abandon_pg_query(cfg, "cancel_probe", hold=0.15)
    time.sleep(0.5)  # the side connection dials and cancels well before the mock would ever reply
    stats = mock.stats("cancel_probe")
    p.check("cancel_probe: abandoning the request sends exactly one CancelRequest",
            stats.get("cancels", 0) == 1, "stats=%r" % stats)

    time.sleep(1.0)  # let the now-orphaned side connection settle
    p.check("cancel_probe: the worker stays healthy after the abort", wfx_healthy(cfg))

# PHASE: pg_resource
def phase_pg_resource(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_resource")

    r = pg_query(cfg, persona="flood_backendkeydata", rtimeout=15.0)
    p.check("flood_backendkeydata: a ParameterStatus flood never lets the handshake finish, so "
            "the connect budget ends it",
            is_errc(r, EP_INTERNAL) or is_errc(r, EP_HANDSHAKE_TIMEOUT), "r=%r" % r)

    r = pg_query(cfg, persona="never_reply_ready", rtimeout=15.0)
    p.check("never_reply_ready: a handshake that never reaches ReadyForQuery times out, not hangs",
            is_errc(r, EP_INTERNAL) or is_errc(r, EP_HANDSHAKE_TIMEOUT), "r=%r" % r)

    r = pg_query(cfg, persona="resource_huge_row", sql="SELECT PGAUDIT_HUGE_ROW", rtimeout=15.0)
    p.check("resource_huge_row: a field over maxMessageBytes is refused, not buffered",
            is_errc(r, EP_INTERNAL), "r=%r" % r)

    r = pg_query(cfg, persona="resource_slow_trickle", rtimeout=15.0)
    p.check("resource_slow_trickle: a handshake trickled past the connect budget times out cleanly",
            is_errc(r, EP_INTERNAL) or is_errc(r, EP_HANDSHAKE_TIMEOUT), "r=%r" % r)

# PHASE: pg_injection
#
# Every query parameter travels through Bind as a typed binary value, never pasted
# into the SQL text (postgres.hpp's own header comment states this as the design).
# These checks make that empirical rather than assumed: the mock records the exact
# SQL text it parsed via Parse (last_sql), so a hostile parameter value showing up
# there at all would mean it leaked into the statement instead of staying data
def phase_pg_injection(ctx):
    cfg, mock = ctx.cfg, ctx.pg_mock
    p = ctx.phase("pg_injection")

    sql = "SELECT $1::text"
    vectors = {
        "classic_drop":  "'; DROP TABLE users; --",
        "union_select":  "' UNION SELECT password FROM pg_shadow--",
        "comment_open":  "'/*",
        "quote_mix":     "\"'; --",
        "backslash":     "\\'; --",
    }
    for name, payload in vectors.items():
        mock.reset("good")
        r = pg_query(cfg, sql=sql, param=payload)
        p.check("%s: the query still succeeds as an ordinary bound parameter" % name,
                is_ok(r) and not r.get("failed"), "r=%r" % r)
        stats = mock.stats("good")
        p.check("%s: the SQL text the server parsed is exactly \"%s\", the payload never "
                "reached it" % (name, sql),
                stats.get("last_sql") == sql, "stats=%r" % stats, security=True)

# PHASE: pg_savepoint
#
# The one place this client composes SQL text from caller input: a savepoint name
# cannot be a bind parameter (Postgres syntax has no placeholder for an identifier),
# so PgSession::Savepoint/RollbackTo/ReleaseSavepoint build "SAVEPOINT <name>" and
# friends directly, guarded only by IsValidIdentifier (wire.hpp). Every hostile name
# here has to be refused before a byte reaches the wire; the mock never even needs
# to see one
def phase_pg_savepoint(ctx):
    cfg = ctx.cfg
    p = ctx.phase("pg_savepoint")

    hostile = {
        "sql_metachars": "x; DROP TABLE users; --",
        "quote":         "x' OR '1'='1",
        "space":         "my savepoint",
        "leading_digit": "1abc",
        "empty":         "",
        "too_long":      "a" * 64,  # MAX_IDENTIFIER_LEN (wire.hpp) is 63
        "backslash":     "x\\y",
        "embedded_nul":  "x\x00y",
    }
    for name, hostile_name in hostile.items():
        r = pg_session(cfg, statements=["SAVEPOINT " + hostile_name], isolation="none", finish="none")
        steps = r.get("steps") or []
        p.check("%s: a hostile savepoint name is refused at the serializer, never sent" % name,
                len(steps) == 1 and steps[0].get("ep") == EP_SERIALIZE, "r=%r" % r, security=True)

    r = pg_session(cfg, statements=["SAVEPOINT valid_name_123",
                                    "ROLLBACK TO SAVEPOINT valid_name_123",
                                    "RELEASE SAVEPOINT valid_name_123"],
                   isolation="read_committed", finish="rollback")
    steps = r.get("steps") or []
    p.check("valid_name_123: Savepoint, RollbackTo and ReleaseSavepoint all succeed on a clean name",
            len(steps) == 3 and all(s.get("ep") == EP_SUCCESS and not s.get("failed") for s in steps),
            "r=%r" % r)

# PHASE: pg_startup
def phase_pg_startup(ctx):
    cfg, mock = ctx.cfg, ctx.pg_mock
    p = ctx.phase("pg_startup")

    # applicationName carries an embedded NUL (main.cpp's Pg_startup_nul_inject).
    # WriteStartup's CStr() (connection.hpp) writes the raw bytes plus its own
    # terminator with no screening for one already inside the string, unlike
    # SmtpOnConnect's HasInjectionBytes check on heloName. This documents that gap
    # rather than asserting it is closed: the raw bytes really do carry the NUL
    mock.reset("startup_nul_inject")
    pg_query(cfg, persona="startup_nul_inject")
    stats = mock.stats("startup_nul_inject")
    raw = bytes.fromhex(stats.get("raw_startup") or "")
    p.check("startup_nul_inject: applicationName's embedded NUL reaches the wire unscreened, "
            "desyncing the startup parameter list that follows it",
            b"evil\x00trailing" in raw, "raw=%r" % raw, security=True)

# PHASE: pg_reconnect
#
# CVE-2018-10915 class: state carried across a reconnect. main.cpp's
# Pg_reconnect_isolation allows 2 attempts; the mock refuses only the first
def phase_pg_reconnect(ctx):
    cfg, mock = ctx.cfg, ctx.pg_mock
    p = ctx.phase("pg_reconnect")

    # maxReconnectAttempts governs the slot's own background reconnect, not a
    # synchronous retry of the request that hit the dead slot: the first request
    # here fails outright, and only a later one lands on the now-reconnected slot
    mock.reset("reconnect_isolation")
    r1 = pg_query(cfg, persona="reconnect_isolation", rtimeout=10.0)
    p.check("reconnect_isolation: the request against the first, refused connection fails",
            is_errc(r1, EP_INTERNAL), "r=%r" % r1)

    time.sleep(2.0)  # past reconnectBackoffBaseSeconds (1s), so the slot has reconnected
    r2 = pg_query(cfg, persona="reconnect_isolation", rtimeout=10.0)
    p.check("reconnect_isolation: a later request on the reconnected slot succeeds cleanly, "
            "with no leakage from the failed attempt",
            is_ok(r2) and not r2.get("failed"), "r=%r" % r2)

    stats = mock.stats("reconnect_isolation")
    p.check("reconnect_isolation: exactly two TCP connection attempts were made, one refused "
            "and one accepted",
            stats.get("connect_attempts") == 2, "stats=%r" % stats)

class ClientAudit(common.Suite):
    name = "client_audit"
    description = "WFX HttpEndpoint, SmtpEndpoint and PostgresEndpoint audit"
    phases = {
        "http_framing":       phase_http_framing,
        "http_statusline":    phase_http_statusline,
        "http_headers":       phase_http_headers,
        "http_chunked":       phase_http_chunked,
        "http_eof":           phase_http_eof,
        "http_desync":        phase_http_desync,
        "http_serialize":     phase_http_serialize,
        "http_limits":        phase_http_limits,
        "http_resource":      phase_http_resource,
        "http_fragmentation": phase_http_fragmentation,
        "http_methods":       phase_http_methods,
        "http_security":      phase_http_security,
        "http_lifecycle":     phase_http_lifecycle,
        "http_metrics":       phase_http_metrics,
        "smtp_handshake":     phase_smtp_handshake,
        "smtp_auth":          phase_smtp_auth,
        "smtp_starttls":      phase_smtp_starttls,
        "smtp_certs":         phase_smtp_certs,
        "smtp_resource":      phase_smtp_resource,
        "smtp_drops":         phase_smtp_drops,
        "smtp_inject":        phase_smtp_inject,
        "pg_framing":         phase_pg_framing,
        "pg_ssl":             phase_pg_ssl,
        "pg_handshake":       phase_pg_handshake,
        "pg_auth":            phase_pg_auth,
        "pg_scram":           phase_pg_scram,
        "pg_wire":            phase_pg_wire,
        "pg_results":         phase_pg_results,
        "pg_types":           phase_pg_types,
        "pg_cache":           phase_pg_cache,
        "pg_stream":          phase_pg_stream,
        "pg_cancel":          phase_pg_cancel,
        "pg_resource":        phase_pg_resource,
        "pg_injection":       phase_pg_injection,
        "pg_savepoint":       phase_pg_savepoint,
        "pg_startup":         phase_pg_startup,
        "pg_reconnect":       phase_pg_reconnect,
    }

    def add_arguments(self, parser):
        parser.add_argument("--http-port", type=int, default=8091,
                            help="HTTP mock port, MUST match UPSTREAM in app/src/main.cpp")

    def configure(self, cfg):
        cfg.http_port = cfg.args.http_port

        # The port is compiled into the app, so a mismatch means the audit drives an
        # endpoint that can never reach the mock
        if cfg.http_port != 8091:
            term.log("runner", term.yellow(
                "NOTE: --http-port=%d must match UPSTREAM in app/src/main.cpp (default 8091), "
                "the port is baked in at compile time" % cfg.http_port))

        cfg.cert_dir = os.path.join(HERE, "certs")
        ensure_smtp_certs(cfg)
        patch_outbound_ca(cfg)

    def setup(self, ctx):
        ctx.resources["http_mock"] = HttpMock(ctx.cfg)
        ctx.http_mock.start()
        ctx.resources["smtp_mock"] = SmtpMock(ctx.cfg)
        ctx.smtp_mock.start()
        ctx.resources["pg_mock"] = PgMock(ctx.cfg)
        ctx.pg_mock.start()

    def before_phases(self, ctx):
        cfg = ctx.cfg

        # EpPrewarm opens its connections eagerly at boot. The mock accepts them but
        # they send nothing, so accepted-minus-served counts the idle prewarmed ones.
        # Snapshot it before any request is driven, or later traffic makes it unreadable
        cfg.prewarm_idle = -1
        for _ in range(50):
            cfg.prewarm_idle = ctx.http_mock.idle_conns()
            if cfg.prewarm_idle >= 3:
                break
            time.sleep(0.1)

        term.log("runner", "idle prewarmed connections at boot: %d" % cfg.prewarm_idle)

        # Without this, every phase fails for the same uninformative reason
        if not is_ok(http_call(cfg, "/ok"), 200, "hello"):
            ctx.phase("preflight").failed(
                "WFX can reach the HTTP mock",
                "is UPSTREAM in app/src/main.cpp pointing at port %d?" % cfg.http_port)
            return False

        r = pg_query(cfg)
        if not (is_ok(r) and not r.get("failed")):
            ctx.phase("preflight").failed(
                "WFX can reach the Postgres mock and complete a SCRAM handshake",
                "r=%r, is Pg_good in app/src/main.cpp pointing at port 8130?" % r)
            return False

    def teardown(self, ctx):
        if "http_mock" in ctx.resources:
            ctx.http_mock.stop()
        if "smtp_mock" in ctx.resources:
            ctx.smtp_mock.stop()
        if "pg_mock" in ctx.resources:
            ctx.pg_mock.stop()

if __name__ == "__main__":
    common.run(ClientAudit)
