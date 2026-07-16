#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX tls_audit: adversarial TLS client audit.
#
# The endpoint audit runs WFX's outbound client PLAINTEXT against a literal-IP mock,
# so the entire TLS client trust decision is untested. This drives WFX's
# HttpEndpoint with EpTlsRequire against a small, hostile TLS mock: an untrusted
# cert, a hostname-mismatched cert, an expired cert, and a downgraded server. Each
# must be refused; a client that accepts any of them is a man-in-the-middle hole.
# Refusal is proven twice: the outbound call errors AND the mock recorded a FAILED
# handshake (the client bailed at the TLS layer, never completing it). Plus: the
# HTTP framing/desync/injection corpus replayed THROUGH TLS, request-timeout under
# TLS, and a raw truncation attack (RST with no close_notify).
#
# Topology:  harness --(TLS)--> WFX(HTTPS) /call --(TLS)--> hostile mock personality
# Exit codes: 0 pass  1 correctness fail / worker died  2 SECURITY finding  3 boot crash

import argparse
import json
import os
import re
import socket
import ssl
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import _audit_common as common

EP_SUCCESS   = 0
EP_SERIALIZE = 10
EP_REQ_TIMEOUT = 13

HERE = os.path.dirname(os.path.abspath(__file__))

# Name, port, cert basename, mock opts, expect ("accept"|"refuse")
PERSONAS = [
    ("good",       8443, "good",       {},                "accept"),
    ("selfsigned", 8444, "selfsigned", {},                "refuse"),
    ("wronghost",  8445, "wronghost",  {},                "refuse"),
    ("expired",    8446, "expired",    {},                "refuse"),
    ("tls12",      8447, "good",       {"maxver": "1.2"}, "refuse"),
]

# Terminal / logging: shared with the other audits, see _audit_common.py
_green, _red, _yellow, _cyan, _bold = common.green, common.red, common.yellow, common.cyan, common.bold
_log, _hdr = common.log, common.hdr
LogFollower = common.LogFollower

# TLS client (verification off, we're driving vectors, not validating certs)
def tls_send(host, port, payload, rtimeout=6.0, ctimeout=4.0, sni="localhost"):
    ctx = ssl._create_unverified_context()
    try:
        raw = socket.create_connection((host, port), timeout=ctimeout)
        s = ctx.wrap_socket(raw, server_hostname=sni)
    except (OSError, ssl.SSLError):
        return None
    try:
        s.sendall(payload); s.settimeout(rtimeout)
        chunks = []
        while True:
            try: d = s.recv(65536)
            except (socket.timeout, OSError): break
            if not d: break
            chunks.append(d)
        return b"".join(chunks)
    except OSError: return None
    finally:
        try: s.close()
        except OSError: pass

_build = common.build_request
_body_of = common.response_body
_status_of = common.response_status

class Cfg: pass

# Certs: small and portable, no chain-building, no OpenSSL-3-only flags
def _ossl(args): return subprocess.run(["openssl"] + args, capture_output=True, text=True)
def _mkcert(args): return subprocess.run(["mkcert"] + args, capture_output=True, text=True)

def ensure_certs(cfg):
    cd = cfg.cert_dir
    os.makedirs(cd, exist_ok=True)
    _mkcert(["-install"])
    caroot = _mkcert(["-CAROOT"]).stdout.strip()
    ca, cakey = os.path.join(caroot, "rootCA.pem"), os.path.join(caroot, "rootCA-key.pem")
    # Pin the client's CA trust to this file directly (see patch_ssl_paths) rather
    # than relying on `mkcert -install` having reached the OS OpenSSL store, which
    # needs sudo on Linux and may silently not happen.
    cfg.ca_path = ca if os.path.exists(ca) else ""

    def p(x): return os.path.join(cd, x)
    avail = set()

    # good: mkcert-trusted, matches both 127.0.0.1 and localhost.
    if _mkcert(["-cert-file", p("good.pem"), "-key-file", p("good-key.pem"),
                "localhost", "127.0.0.1", "::1"]).returncode == 0:
        avail.add("good")

    # selfsigned: untrusted, unknown CA.
    if _ossl(["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", p("selfsigned-key.pem"),
              "-out", p("selfsigned.pem"), "-days", "365", "-subj", "/CN=127.0.0.1",
              "-addext", "subjectAltName=IP:127.0.0.1"]).returncode == 0:
        avail.add("selfsigned")

    # wronghost: mkcert-trusted, but only valid for a different name -> hostname
    # mismatch when the client connects to 127.0.0.1.
    if _mkcert(["-cert-file", p("wronghost.pem"), "-key-file", p("wronghost-key.pem"),
                "evil.example"]).returncode == 0:
        avail.add("wronghost")

    # expired: trusted CA, but `-days -1` backdates notAfter to yesterday. Portable
    # across OpenSSL versions (unlike -not_before/-not_after, OpenSSL 3.0+ only).
    if os.path.exists(ca) and os.path.exists(cakey):
        csr = p("expired.csr")
        ext = p("expired-ext.cnf")
        with open(ext, "w") as f: f.write("subjectAltName=IP:127.0.0.1\n")
        made_csr = _ossl(["req", "-new", "-newkey", "rsa:2048", "-nodes",
                          "-keyout", p("expired-key.pem"), "-subj", "/CN=127.0.0.1",
                          "-out", csr]).returncode == 0
        signed = _ossl(["x509", "-req", "-in", csr, "-CA", ca, "-CAkey", cakey,
                        "-CAcreateserial", "-out", p("expired.pem"),
                        "-days", "-1", "-extfile", ext])
        if made_csr and signed.returncode == 0 and os.path.exists(p("expired.pem")):
            avail.add("expired")
        else:
            _log("certs", _yellow("expired cert unavailable: %s" % signed.stderr.strip()[:120]))
    else:
        _log("certs", _yellow("mkcert CA key not found, 'expired' vector skipped"))

    if "good" in avail:
        avail.add("tls12")  # reuses the good cert

    _log("certs", _green("personas available: %s" % ", ".join(sorted(avail))))
    cfg.avail = avail
    if "good" not in avail:
        raise RuntimeError("could not generate the 'good' cert (mkcert broken?)")

def persona_available(cfg, cert):
    return cert in cfg.avail

def patch_ssl_paths(cfg):
    """Point the server cert at the mkcert 'good' leaf, and pin the outbound
    client's CA trust explicitly at the mkcert root instead of leaving
    ca_cert_path empty (see ensure_certs). Doesn't weaken any refusal test:
    hostname/expiry/downgrade checks are independent of the trust anchor."""
    toml = os.path.join(cfg.app_dir, "wfx.toml")
    with open(toml) as f: s = f.read()
    good, key = os.path.join(cfg.cert_dir, "good.pem"), os.path.join(cfg.cert_dir, "good-key.pem")
    ca = getattr(cfg, "ca_path", "")
    s = re.sub(r'(?m)^(\s*cert_path\s*=\s*)"[^"]*"', lambda m: m.group(1) + '"%s"' % good, s)
    s = re.sub(r'(?m)^(\s*key_path\s*=\s*)"[^"]*"',  lambda m: m.group(1) + '"%s"' % key, s)
    if ca:
        s = re.sub(r'(?m)^(\s*ca_cert_path\s*=\s*)"[^"]*"', lambda m: m.group(1) + '"%s"' % ca, s)
    with open(toml, "w") as f: f.write(s)
    _log("patch", "server cert -> %s | client CA trust -> %s" % (good, ca or "(system store)"))

# Mock context
class Mock:
    def __init__(self, cfg):
        self.cfg = cfg
        self.proc = None
        self._reader = None
        # persona -> port, so control-plane calls (stats/stage) always target the
        # SAME listener the harness is about to ask WFX to hit.
        self.ports = {name: port for name, port, cert, opts, expect in PERSONAS
                     if persona_available(cfg, cert)}

    def start(self):
        cmd = [sys.executable, os.path.join(HERE, "tls_upstream.py"), "--host", self.cfg.host]
        cd = self.cfg.cert_dir
        for name, port, cert, opts, expect in PERSONAS:
            if not persona_available(self.cfg, cert): continue
            spec = "name=%s,port=%d,cert=%s,key=%s" % (name, port, os.path.join(cd, cert + ".pem"),
                                                       os.path.join(cd, cert + "-key.pem"))
            for k, v in opts.items(): spec += ",%s=%s" % (k, v)
            cmd += ["--listen", spec]
        _log("mock", "starting %d TLS listeners" % len(self.ports))
        # Stream the mock's own output instead of discarding it: a listener thread
        # that fails to bind or load its cert prints a traceback to stderr, and
        # silently swallowing that makes a broken TEST FIXTURE indistinguishable
        # from a real WFX bug.
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()

        good_port = self.ports.get("good", 8443)
        deadline = time.time() + 5.0
        while time.time() < deadline:
            try:
                s = socket.create_connection((self.cfg.host, good_port), timeout=1.0); s.close()
                _log("mock", _green("TLS mock up")); return
            except OSError: time.sleep(0.1)
        raise RuntimeError("TLS mock never came up on :%d, see [mock] output above" % good_port)

    def _drain(self):
        try:
            for line in self.proc.stdout:
                line = line.rstrip()
                if line: print("%s %s" % (_cyan("[mock]"), line), flush=True)
        except (OSError, ValueError):
            pass

    def stats(self, name):
        port = self.ports.get(name, 8443)
        raw = tls_send(self.cfg.host, port, _build("GET", "/ctl/stats/%s" % name))
        try: return [int(x) for x in _body_of(raw).split()]   # [handshakes, hs_fail, requests, resumed]
        except Exception: return [-1, -1, -1, -1]

    def stage(self, sid, blob, keep=True, ep="good"):
        port = self.ports.get(ep, 8443)
        tls_send(self.cfg.host, port, _build("POST", "/ctl/stage",
                 {"X-Id": sid, "X-Keep": "1" if keep else "0"}, blob))

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try: self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired: self.proc.kill()

# Server context
class Server:
    def __init__(self, cfg): self.cfg = cfg; self._up = False
    def start(self):
        # Relative app name, cwd at the audit dir: `wfx run` with an ABSOLUTE app
        # path from a different cwd builds and logs "Server running" but the
        # detached daemon then fails to bind.
        app = os.path.basename(self.cfg.app_dir.rstrip("/"))
        cmd = [self.cfg.wfx, "run", app, "--use-https", "--https-port-override",
               "--port", str(self.cfg.port), "--detach"]
        _log("server", "starting (cwd=%s): %s" % (HERE, " ".join(cmd)))
        r = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("wfx run failed (rc=%d): %s %s" % (r.returncode, r.stdout.strip(), r.stderr.strip()))
        self._up = True; _log("server", _green("detached OK"))
    def wait_ready(self):
        _log("server", "waiting for /health (HTTPS) …"); t0 = time.time()
        while time.time() - t0 < self.cfg.ready_timeout:
            raw = tls_send(self.cfg.host, self.cfg.port, _build("GET", "/health"), rtimeout=1.5, ctimeout=1.5)
            if raw and _status_of(raw) == 200:
                _log("server", _green("up in %.1fs" % (time.time() - t0))); return True
            time.sleep(0.3)
        return False
    def alive(self):
        raw = tls_send(self.cfg.host, self.cfg.port, _build("GET", "/health"), rtimeout=2.5, ctimeout=2.5)
        return bool(raw) and _status_of(raw) == 200
    def stop(self):
        if not self._up: return
        _log("server", "stopping …")
        subprocess.run([self.cfg.wfx, "control", "stop", "app"], capture_output=True, text=True)

# Drive + Predicates
def drive(cfg, path="/ok", ep="good", rtimeout=8.0):
    raw = tls_send(cfg.host, cfg.port, _build("GET", "/call", {"X-Ep": ep, "X-Path": path}), rtimeout=rtimeout)
    if not raw or _status_of(raw) != 200: return None
    try: return json.loads(_body_of(raw))
    except Exception: return None

def inject(cfg, mode, body, rtimeout=8.0):
    raw = tls_send(cfg.host, cfg.port, _build("POST", "/inject", {"X-Inject": mode}, body), rtimeout=rtimeout)
    if not raw or _status_of(raw) != 200: return None
    try: return json.loads(_body_of(raw))
    except Exception: return None

def is_ok(r, status=None, body=None):
    if not r or r.get("ep") != EP_SUCCESS: return False
    if status is not None and r.get("status") != status: return False
    if body is not None and r.get("body") != body: return False
    return True
def is_err(r): return bool(r) and r.get("ep") != EP_SUCCESS
def is_errc(r, code): return bool(r) and r.get("ep") == code

_sid = [0]
def _next_sid():
    _sid[0] += 1; return "s%d" % _sid[0]
def drive_staged(cfg, mock, blob, ep="good", keep=True):
    sid = _next_sid(); mock.stage(sid, blob, keep, ep=ep)
    return drive(cfg, "/raw/%s" % sid, ep=ep)

# Results: shared with the other audits, see _audit_common.py
Results = common.Results
check = common.check

# Phases
def phase_handshake(cfg, mock, results):
    b = results.phase("handshake")
    check(b, "TLS GET /ok round-trips", is_ok(drive(cfg, "/ok"), 200, "hello"), "got %r" % drive(cfg, "/ok"))
    check(b, "TLS empty body",          is_ok(drive(cfg, "/empty"), 200, ""))
    check(b, "TLS large body (4000)",   (lambda r: is_ok(r, 200) and r.get("bodylen") == 4000)(drive(cfg, "/cl/4000")))
    check(b, "TLS chunked",             is_ok(drive(cfg, "/chunked"), 200, "abcde"))
    check(b, "TLS keep-alive reuse x10", all(is_ok(drive(cfg, "/ok"), 200, "hello") for _ in range(10)))
    hs, hf, rq, _ = mock.stats("good")
    check(b, "good listener completed handshakes", hs >= 1, "stats good=%r" % [hs, hf, rq])

def phase_verify(cfg, mock, results):
    b = results.phase("verify")
    labels = {
        "selfsigned": "untrusted self-signed cert refused",
        "wronghost":  "hostname-mismatch cert refused",
        "expired":    "expired cert refused",
    }
    for name, port, cert, opts, expect in PERSONAS:
        if expect != "refuse" or name == "tls12":
            continue
        if not persona_available(cfg, cert):
            check(b, labels.get(name, name) + " (skipped, no cert)", True); continue
        r = drive(cfg, "/ok", ep=name, rtimeout=12)
        check(b, labels[name], is_err(r),
              "client ACCEPTED it (ep=%r), MitM possible" % (r and r.get("ep")), security=True)
        hs, hf, rq, _ = mock.stats(name)
        # mock.stats() itself dials this same port with an UNVERIFIED TLS client to
        # fetch the counters, so `hs` always includes that one successful handshake
        # regardless of what the WFX client did, it is not evidence the client
        # completed a handshake. Only hs_fail speaks to the client here.
        check(b, name + ": client bailed at TLS layer", hf >= 1,
              "handshakes=%d hs_fail=%d (want at least one failed handshake)" % (hs, hf), security=True)

def phase_protocol(cfg, mock, results):
    b = results.phase("protocol")
    if not persona_available(cfg, "good"):
        check(b, "TLS 1.2 downgrade refused (skipped)", True); return
    r = drive(cfg, "/ok", ep="tls12", rtimeout=12)
    check(b, "TLS 1.2 downgrade refused", is_err(r),
          "client accepted a TLS 1.2 downgrade (ep=%r)" % (r and r.get("ep")), security=True)

def phase_framing(cfg, mock, results):
    b = results.phase("framing")
    ok = [
        ("CL body",         b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", 200, "hi"),
        ("CL zero",         b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 200, ""),
        ("chunked",         b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n", 200, "abc"),
        ("chunked multi",   b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n1\r\nb\r\n0\r\n\r\n", 200, "ab"),
        ("100 then 200",    b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nafter", 200, "after"),
        ("204 no body",     b"HTTP/1.1 204 No Content\r\n\r\n", 204, ""),
        ("dup CL equal",    b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nhi", 200, "hi"),
    ]
    for name, blob, status, expect in ok:
        check(b, "TLS framing ok: %s" % name, is_ok(drive_staged(cfg, mock, blob), status, expect))

    bad = [
        ("bad status HTTP/2",  b"HTTP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n"),
        ("obs-fold header",    b"HTTP/1.1 200 OK\r\nX: a\r\n b\r\nContent-Length: 2\r\n\r\nhi"),
        ("dup CL differ",      b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nhi"),
        ("CL + TE",             b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\nhi"),
        ("chunk non-hex",      b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nabc\r\n0\r\n\r\n"),
    ]
    for name, blob in bad:
        sec = name in ("obs-fold header", "dup CL differ", "CL + TE")
        check(b, "TLS framing bad: %s" % name, is_err(drive_staged(cfg, mock, blob)),
              "malformed framing accepted over TLS", security=sec)

    trunc = [
        ("mid status",  b"HTTP/1.1 200"),
        ("mid CL body", b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"),
        ("mid chunk",   b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab"),
    ]
    for name, blob in trunc:
        check(b, "TLS eof: %s" % name, is_err(drive_staged(cfg, mock, blob, keep=False)))

def phase_desync(cfg, mock, results):
    b = results.phase("desync")
    poisons = [
        ("204 + body",       b"HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\nhello"),
        ("trailing smuggle", b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ngoodHTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nSMUGGLE"),
    ]
    for name, blob in poisons:
        drive_staged(cfg, mock, blob, keep=True)
        clean = all(is_ok(drive(cfg, "/ok"), 200, "hello") for _ in range(8))
        check(b, "no TLS-pool poison: %s" % name, clean, "pooled TLS conn corrupted after %s" % name, security=True)
    r = drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ngoodHTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nSMUGGLE", keep=True)
    check(b, "smuggled body not delivered", (r is None) or (r.get("body") != "SMUGGLE"), "delivered smuggled bytes: %r" % r, security=True)

def phase_inject(cfg, mock, results):
    b = results.phase("inject")
    path_vec = [
        ("CRLF path", b"/a\r\nX-Smuggle: 1"),
        ("bare LF",   b"/a\nX-Smuggle: 1"),
        ("bare CR",   b"/a\rX-Smuggle: 1"),
        ("NUL",       b"/a\x00b"),
        ("full req",  b"/a\r\nHost: evil\r\nGET /x HTTP/1.1\r\n"),
    ]
    for name, pl in path_vec:
        check(b, "reject path: %s" % name, is_errc(inject(cfg, "path", pl), EP_SERIALIZE),
              "expected serialize-err, got %r" % inject(cfg, "path", pl), security=True)
    hdr_vec = [
        ("CRLF value", b"X-E: a\r\nX-Smuggle: b"),
        ("CRLF name",  b"X-E\r\nX: b"),
        ("NUL value",  b"X-E: a\x00b"),
    ]
    for name, pl in hdr_vec:
        check(b, "reject header: %s" % name, is_errc(inject(cfg, "header", pl), EP_SERIALIZE),
              "expected serialize-err", security=True)
    check(b, "clean path accepted",   is_ok(inject(cfg, "path", b"/ok"), 200, "hello"))
    check(b, "clean header accepted", is_ok(inject(cfg, "header", b"X-Ok: fine"), 200, "hello"))

def phase_resource(cfg, mock, results):
    b = results.phase("resource")
    t0 = time.time()
    check(b, "slow response -> timeout", is_err(drive(cfg, "/slow", ep="fast", rtimeout=18)), "elapsed %.1fs" % (time.time() - t0))
    r = drive(cfg, "/truncate", ep="good", rtimeout=12)
    check(b, "truncation not delivered as success",
          (r is None) or (r.get("ep") != EP_SUCCESS) or (r.get("body") != "partial-body-then-brutal-reset"),
          "truncated body delivered as complete (ep=%r body=%r)" % (r and r.get("ep"), r and r.get("body")), security=True)

# WFX's outbound client caches one TLS session per configured HttpEndpoint (see
# http_openssl.cpp's NewClientSessionCallback / EndpointMetadata::cachedTlsSession) and
# offers it back on the next connection to that same endpoint. A live TCP+TLS connection
# just gets kept alive and reused by the connection pool though, so resumption only has
# a chance to fire once the pooled connection actually dies and a new one has to be
# opened - /truncate resets the connection, which is how we force that here.
def phase_resumption(cfg, mock, results):
    b = results.phase("resumption")
    if not persona_available(cfg, "good"):
        check(b, "TLS session resumption (skipped, no cert)", True); return

    # Warm-up: cache is empty, this must be a full handshake
    r = drive(cfg, "/ok", ep="good", rtimeout=12)
    check(b, "warm-up call succeeds", is_ok(r, 200, "hello"), "got %r" % r)

    # Kill every pooled connection (connLimit=4 for 'good' - more than 4 resets makes
    # it very likely every pooled slot actually got cycled at least once)
    for _ in range(6):
        drive(cfg, "/truncate", ep="good", rtimeout=12)

    # Each of these must reconnect (or find an already-reconnected pool slot) and still
    # work; at least one of them should land on a fresh connection that resumes the
    # session cached by the warm-up call above
    reconnected = [drive(cfg, "/ok", ep="good", rtimeout=12) for _ in range(6)]
    check(b, "calls after forced reconnect still succeed",
          all(is_ok(r, 200, "hello") for r in reconnected), "got %r" % reconnected)

    hs, hf, rq, resumed = mock.stats("good")
    check(b, "session resumed at least once after reconnect", resumed >= 1,
          "stats good=%r (handshakes, hs_fail, requests, resumed)" % [hs, hf, rq, resumed])


PHASES = {"handshake": phase_handshake, "verify": phase_verify, "protocol": phase_protocol,
          "framing": phase_framing, "desync": phase_desync, "inject": phase_inject, "resource": phase_resource,
          "resumption": phase_resumption}

# Report
def report(results, booted, server_alive):
    _hdr("REPORT")
    passed, total, sec, fail = common.format_report(results)
    print()
    if not booted:
        _log("harness", _red("WFX never reached /health, boot crash. See [wfx-crash:*] above."))
    _log("harness", "server alive at end: %s" % (_green("yes") if server_alive else _red("NO")))
    print(_bold("  TOTAL  %s   security: %s   other: %s" % (
        _green("%d/%d passed" % (passed, total)),
        _red(str(sec)) if sec else _green("0"), _red(str(fail)) if fail else _green("0"))))
    if not booted: return 3
    if sec: return 2
    if not server_alive or fail: return 1
    return 0

# Main
def main():
    ap = argparse.ArgumentParser(description="WFX adversarial TLS audit")
    common.add_common_args(ap, PHASES)
    ap.set_defaults(app_dir=os.path.join(HERE, "app"))
    args = ap.parse_args()

    if args.ci:
        common.enable_ci_mode()

    if args.list_phases:
        for p in PHASES: print(p)
        return 0

    cfg = Cfg()
    cfg.host = args.host; cfg.port = args.port; cfg.wfx = args.wfx
    cfg.app_dir = args.app_dir; cfg.ready_timeout = args.ready_timeout
    cfg.cert_dir = os.path.join(HERE, "certs")

    ensure_certs(cfg)
    patch_ssl_paths(cfg)

    results = Results()
    mock = Mock(cfg); server = Server(cfg); follower = LogFollower(cfg.app_dir, mode=args.wfx_logs)
    booted = False
    try:
        mock.start(); server.start(); follower.start()
        booted = server.wait_ready()
        if not booted:
            _log("harness", _red("WFX did not answer /health within %ds, see WFX logs above" % cfg.ready_timeout))
            time.sleep(0.5)
            return report(results, booted, False)

        _log("harness", _green("booted"))
        run = list(PHASES) if args.phase == "all" else [args.phase]
        for name in run:
            _hdr("PHASE: " + name)
            common.gh_group("phase: " + name)
            try: PHASES[name](cfg, mock, results)
            except Exception as e:
                _log("harness", _red("phase %s crashed: %r" % (name, e)))
                results.phase(name).append(("phase-exception", False, False, repr(e)))
            common.gh_endgroup()
            if not server.alive():
                _log("harness", _red("WFX worker NOT responding after phase '%s', waiting for revival, see WFX logs" % name))
                t0 = time.time()
                while time.time() - t0 < 15.0 and not server.alive():
                    time.sleep(0.3)
                if not server.alive():
                    _log("harness", _red("worker did not come back within 15s"))
        return report(results, booted, server.alive())
    finally:
        follower.stop(); server.stop(); mock.stop()

if __name__ == "__main__":
    try: sys.exit(main())
    except KeyboardInterrupt: sys.exit(130)
