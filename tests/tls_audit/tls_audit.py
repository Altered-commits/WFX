#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX tls_audit: adversarial TLS client audit
#
# The endpoint audit runs WFX's outbound client PLAINTEXT against a literal-IP mock,
# so the entire TLS client trust decision is untested. This drives WFX's
# HttpEndpoint with EpTlsRequire against a small, hostile TLS mock: an untrusted
# cert, a hostname-mismatched cert, an expired cert, and a downgraded server. Each
# must be refused; a client that accepts any of them is a man-in-the-middle hole
# Refusal is proven twice: the outbound call errors AND the mock recorded a FAILED
# handshake (the client bailed at the TLS layer, never completing it). Plus: the
# HTTP framing/desync/injection corpus replayed THROUGH TLS, request-timeout under
# TLS, and a raw truncation attack (RST with no close_notify)
#
# Topology:  audit --(TLS)--> WFX(HTTPS) /call --(TLS)--> hostile mock personality
# Exit codes: 0 pass  1 correctness fail / worker died  2 SECURITY finding  3 boot crash

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

_green, _red, _yellow = term.green, term.red, term.yellow

HERE = os.path.dirname(os.path.abspath(__file__))

# Mirrors EndpointStatus in shared/abis/types.hpp, keep in sync
EP_SUCCESS     = 0
EP_SERIALIZE   = 11
EP_REQ_TIMEOUT = 14

# Name, port, cert basename, mock opts, expect ("accept"|"refuse")
PERSONAS = [
    ("good",       8443, "good",       {},                "accept"),
    ("selfsigned", 8444, "selfsigned", {},                "refuse"),
    ("wronghost",  8445, "wronghost",  {},                "refuse"),
    ("expired",    8446, "expired",    {},                "refuse"),
    ("tls12",      8447, "good",       {"maxver": "1.2"}, "refuse"),
    # Plaintext until the client asks to upgrade, then TLS with the good cert
    # The upgrade phase toggles attacker plaintext on and off at runtime, see Mock.upgrade_inject
    ("upgrade",    8448, "good",       {"kind": "upgrade"}, "accept"),
]

# Transport
# Certificate verification is off on the audit side: we drive vectors at WFX, and WFX's own trust
# decision is what is under test
_build     = net.request
_body_of   = net.body
_status_of = net.status

# WFX runs this whole suite with client_ca_path set (see patch_ssl_paths), so every call below
# needs a client cert or WFX refuses the handshake before HTTP is even in play. Defaulting to the
# trusted one here means every existing call site gets it for free, and mTLS refusal vectors pass
# an explicit certfile/keyfile (or None) to override it
CLIENT_CERT = os.path.join(HERE, "certs", "client-good.pem")
CLIENT_KEY  = os.path.join(HERE, "certs", "client-good-key.pem")

def tls_send(host, port, payload, rtimeout=6.0, ctimeout=4.0, sni="localhost",
            certfile=CLIENT_CERT, keyfile=CLIENT_KEY):
    return net.send(host, port, payload, rtimeout=rtimeout, ctimeout=ctimeout, tls=True, sni=sni,
                    certfile=certfile, keyfile=keyfile)

# Certs: small and portable, no chain-building, no OpenSSL-3-only flags
def _ossl(args):
    return subprocess.run(["openssl"] + args, capture_output=True, text=True)

def _mkcert(args):
    return subprocess.run(["mkcert"] + args, capture_output=True, text=True)

def ensure_certs(cfg):
    cd = cfg.cert_dir
    os.makedirs(cd, exist_ok=True)
    _mkcert(["-install"])
    caroot = _mkcert(["-CAROOT"]).stdout.strip()
    ca, cakey = os.path.join(caroot, "rootCA.pem"), os.path.join(caroot, "rootCA-key.pem")
    # Pin the client's CA trust to this file directly (see patch_ssl_paths) rather
    # than relying on `mkcert -install` having reached the OS OpenSSL store, which
    # needs sudo on Linux and may silently not happen
    cfg.ca_path = ca if os.path.exists(ca) else ""

    def p(x):
        return os.path.join(cd, x)
    avail = set()

    # good: mkcert-trusted, matches both 127.0.0.1 and localhost
    if _mkcert(["-cert-file", p("good.pem"), "-key-file", p("good-key.pem"),
                "localhost", "127.0.0.1", "::1"]).returncode == 0:
        avail.add("good")

    # selfsigned: untrusted, unknown CA
    if _ossl(["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", p("selfsigned-key.pem"),
              "-out", p("selfsigned.pem"), "-days", "365", "-subj", "/CN=127.0.0.1",
              "-addext", "subjectAltName=IP:127.0.0.1"]).returncode == 0:
        avail.add("selfsigned")

    # wronghost: mkcert-trusted, but only valid for a different name -> hostname
    # mismatch when the client connects to 127.0.0.1
    if _mkcert(["-cert-file", p("wronghost.pem"), "-key-file", p("wronghost-key.pem"),
                "evil.example"]).returncode == 0:
        avail.add("wronghost")

    # expired: trusted CA, but `-days -1` backdates notAfter to yesterday. Portable
    # across OpenSSL versions (unlike -not_before/-not_after, OpenSSL 3.0+ only)
    if os.path.exists(ca) and os.path.exists(cakey):
        csr = p("expired.csr")
        ext = p("expired-ext.cnf")
        with open(ext, "w") as f:
            f.write("subjectAltName=IP:127.0.0.1\n")
        made_csr = _ossl(["req", "-new", "-newkey", "rsa:2048", "-nodes",
                          "-keyout", p("expired-key.pem"), "-subj", "/CN=127.0.0.1",
                          "-out", csr]).returncode == 0
        signed = _ossl(["x509", "-req", "-in", csr, "-CA", ca, "-CAkey", cakey,
                        "-CAcreateserial", "-out", p("expired.pem"),
                        "-days", "-1", "-extfile", ext])
        if made_csr and signed.returncode == 0 and os.path.exists(p("expired.pem")):
            avail.add("expired")
        else:
            term.log("certs", _yellow("expired cert unavailable: %s" % signed.stderr.strip()[:120]))
    else:
        term.log("certs", _yellow("mkcert CA key not found, 'expired' vector skipped"))

    if "good" in avail:
        avail.add("tls12")  # reuses the good cert

    # vvv Client certificates, presented BACK to WFX for the inbound mTLS phase vvv
    # This suite's own mkcert CA plays both roles: WFX already trusts it for verifying itself as an
    # outbound client (outbound_ca_path above), and now also trusts it for verifying US as an
    # inbound client (client_ca_path, see patch_ssl_paths). Separate OpenSSL contexts, no conflict
    if os.path.exists(ca) and os.path.exists(cakey):
        # client-good: signed by the trusted CA, must be ACCEPTED. tls_send() defaults to this,
        # so its absence would silently break every other phase too - required, not best-effort
        if _ossl(["req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", p("client-good-key.pem"),
                  "-subj", "/CN=mtls-good-client", "-out", p("client-good.csr")]).returncode == 0:
            signed = _ossl(["x509", "-req", "-in", p("client-good.csr"), "-CA", ca, "-CAkey", cakey,
                            "-CAcreateserial", "-out", p("client-good.pem"), "-days", "365"])
            if signed.returncode == 0:
                avail.add("client-good")
        if "client-good" not in avail:
            raise RuntimeError("could not generate the 'client-good' cert (mkcert CA broken?)")

        # client-expired: same trusted CA, backdated notAfter -> must be REFUSED
        if _ossl(["req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", p("client-expired-key.pem"),
                  "-subj", "/CN=mtls-expired-client", "-out", p("client-expired.csr")]).returncode == 0:
            signed = _ossl(["x509", "-req", "-in", p("client-expired.csr"), "-CA", ca, "-CAkey", cakey,
                            "-CAcreateserial", "-out", p("client-expired.pem"), "-days", "-1"])
            if signed.returncode == 0:
                avail.add("client-expired")

        # client-viaint: leaf signed by an intermediate CA, itself signed by a DEDICATED root - not
        # the mkcert one. Verified independently with a plain `openssl verify` (not assumed): the
        # mkcert root is built with pathlen:0, so it refuses to ever be the root of an intermediate
        # CA by mkcert's own design - reusing it here would fail regardless of what WFX does. WFX
        # only ever loads the roots it's given, never fetches a missing intermediate itself - a
        # client sending the leaf alone must be REFUSED, leaf+intermediate together must be
        # ACCEPTED. Proves real chain building, not just "an issuer name that looks familiar"
        chainroot_ok = _ossl(["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", p("chainroot-key.pem"),
                             "-out", p("chainroot.pem"), "-days", "365", "-subj", "/CN=mtls-chain-test-root",
                             "-addext", "basicConstraints=critical,CA:true,pathlen:1",
                             "-addext", "keyUsage=critical,keyCertSign,cRLSign"]).returncode == 0

        if chainroot_ok:
            int_ext = p("int-ext.cnf")
            with open(int_ext, "w") as f:
                f.write("basicConstraints=critical,CA:true\nkeyUsage=critical,keyCertSign,cRLSign\n")
            if _ossl(["req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", p("client-int-key.pem"),
                      "-subj", "/CN=mtls-test-intermediate-ca", "-out", p("client-int.csr")]).returncode == 0:
                signed_int = _ossl(["x509", "-req", "-in", p("client-int.csr"), "-CA", p("chainroot.pem"),
                                    "-CAkey", p("chainroot-key.pem"), "-CAcreateserial",
                                    "-out", p("client-int.pem"), "-days", "365", "-extfile", int_ext])
                if signed_int.returncode == 0 and _ossl(
                    ["req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", p("client-viaint-key.pem"),
                     "-subj", "/CN=mtls-viaint-client", "-out", p("client-viaint.csr")]).returncode == 0:
                    signed_leaf = _ossl(["x509", "-req", "-in", p("client-viaint.csr"), "-CA", p("client-int.pem"),
                                        "-CAkey", p("client-int-key.pem"), "-CAcreateserial",
                                        "-out", p("client-viaint.pem"), "-days", "365"])
                    if signed_leaf.returncode == 0:
                        avail.add("client-viaint")
                        with open(p("client-viaint-chain.pem"), "w") as out:
                            out.write(open(p("client-viaint.pem")).read())
                            out.write(open(p("client-int.pem")).read())
                        avail.add("client-viaint-chain")

                        # client_ca_path must trust BOTH roots at once: the mkcert one (client-good/
                        # -expired/-otherca's counterpart) and this dedicated chain-test one
                        with open(p("client-ca-bundle.pem"), "w") as out:
                            out.write(open(ca).read())
                            out.write(open(p("chainroot.pem")).read())
                        cfg.client_ca_bundle = p("client-ca-bundle.pem")
    else:
        term.log("certs", _yellow("mkcert CA key not found, mTLS client-cert vectors skipped"))

    # client-otherca: signed by a WHOLLY DIFFERENT throwaway CA, never loaded into client_ca_path
    # -> must be REFUSED (untrusted issuer, independent of the trusted-CA vectors above)
    if _ossl(["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", p("otherca-key.pem"),
              "-out", p("otherca.pem"), "-days", "365", "-subj", "/CN=mtls-other-ca"]).returncode == 0:
        if _ossl(["req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", p("client-otherca-key.pem"),
                  "-subj", "/CN=mtls-otherca-client", "-out", p("client-otherca.csr")]).returncode == 0:
            signed = _ossl(["x509", "-req", "-in", p("client-otherca.csr"), "-CA", p("otherca.pem"),
                            "-CAkey", p("otherca-key.pem"), "-CAcreateserial",
                            "-out", p("client-otherca.pem"), "-days", "365"])
            if signed.returncode == 0:
                avail.add("client-otherca")

    # client-selfsigned: self-signed, no CA at all -> must be REFUSED
    if _ossl(["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", p("client-selfsigned-key.pem"),
              "-out", p("client-selfsigned.pem"), "-days", "365",
              "-subj", "/CN=mtls-selfsigned-client"]).returncode == 0:
        avail.add("client-selfsigned")

    term.log("certs", _green("personas available: %s" % ", ".join(sorted(avail))))
    cfg.avail = avail
    if "good" not in avail:
        raise RuntimeError("could not generate the 'good' cert (mkcert broken?)")

def persona_available(cfg, cert):
    return cert in cfg.avail

def patch_ssl_paths(cfg):
    """Point the server cert at the mkcert 'good' leaf, and pin the outbound
    client's CA trust explicitly at the mkcert root instead of leaving
    outbound_ca_path empty (see ensure_certs). Doesn't weaken any refusal test:
    hostname/expiry/downgrade checks are independent of the trust anchor.

    Also turns inbound mTLS ON for the whole run (client_ca_path -> the mkcert root, bundled with
    the dedicated chain-test root when that vector is available), which is why tls_send() defaults
    to presenting a client cert - see CLIENT_CERT above."""
    toml = os.path.join(cfg.app_dir, "wfx.toml")
    with open(toml) as f:
        s = f.read()
    good, key = os.path.join(cfg.cert_dir, "good.pem"), os.path.join(cfg.cert_dir, "good-key.pem")
    ca = getattr(cfg, "ca_path", "")
    s = re.sub(r'(?m)^(\s*cert_path\s*=\s*)"[^"]*"', lambda m: m.group(1) + '"%s"' % good, s)
    s = re.sub(r'(?m)^(\s*key_path\s*=\s*)"[^"]*"',  lambda m: m.group(1) + '"%s"' % key, s)
    if ca:
        s = re.sub(r'(?m)^(\s*outbound_ca_path\s*=\s*)"[^"]*"', lambda m: m.group(1) + '"%s"' % ca, s)
        if "client-good" in cfg.avail:
            client_ca = getattr(cfg, "client_ca_bundle", "") or ca
            s = re.sub(r'(?m)^(\s*client_ca_path\s*=\s*)"[^"]*"', lambda m: m.group(1) + '"%s"' % client_ca, s)
    with open(toml, "w") as f:
        f.write(s)
    term.log("patch", "server cert -> %s | client CA trust -> %s | inbound mTLS -> %s"
            % (good, ca or "(system store)", "on" if "client-good" in cfg.avail else "off"))

# Mock context
class Mock:
    def __init__(self, cfg):
        self.cfg = cfg
        self.proc = None
        self._reader = None
        # persona -> port, so control-plane calls (stats/stage) always target the
        # SAME listener the audit is about to ask WFX to hit
        self.ports = {name: port for name, port, cert, opts, expect in PERSONAS
                     if persona_available(cfg, cert)}

    def start(self):
        cmd = [sys.executable, os.path.join(HERE, "tls_upstream.py"), "--host", self.cfg.host]
        cd = self.cfg.cert_dir
        for name, port, cert, opts, expect in PERSONAS:
            if not persona_available(self.cfg, cert):
                continue
            spec = "name=%s,port=%d,cert=%s,key=%s" % (name, port, os.path.join(cd, cert + ".pem"),
                                                       os.path.join(cd, cert + "-key.pem"))
            for k, v in opts.items():
                spec += ",%s=%s" % (k, v)
            cmd += ["--listen", spec]
        term.log("mock", "starting %d TLS listeners" % len(self.ports))
        # Stream the mock's own output instead of discarding it: a listener thread
        # that fails to bind or load its cert prints a traceback to stderr, and
        # silently swallowing that makes a broken TEST FIXTURE indistinguishable
        # from a real WFX bug
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()

        good_port = self.ports.get("good", 8443)
        deadline = time.time() + 5.0
        while time.time() < deadline:
            try:
                s = socket.create_connection((self.cfg.host, good_port), timeout=1.0)
                s.close()
                term.log("mock", _green("TLS mock up"))
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("TLS mock never came up on :%d, see [mock] output above" % good_port)

    def _drain(self):
        try:
            for line in self.proc.stdout:
                line = line.rstrip()
                if line:
                    print("%s %s" % (term.cyan("[mock]"), line), flush=True)
        except (OSError, ValueError):
            pass

    def stats(self, name):
        port = self.ports.get(name, 8443)
        raw = tls_send(self.cfg.host, port, _build("GET", "/ctl/stats/%s" % name))
        try:
            return [int(x) for x in _body_of(raw).split()]   # [handshakes, hs_fail, requests, resumed]
        except Exception:
            return [-1, -1, -1, -1]

    def stage(self, sid, blob, keep=True, ep="good"):
        port = self.ports.get(ep, 8443)
        tls_send(self.cfg.host, port, _build("POST", "/ctl/stage",
                 {"X-Id": sid, "X-Keep": "1" if keep else "0"}, blob))

    def upgrade_inject(self, on):
        """Turns the pre-upgrade plaintext injection on the 'upgrade' listener on or off.

        Goes through the 'good' listener because the upgrade one speaks its own protocol, not
        HTTP. Both live in one process, so the flag is shared.
        """
        port = self.ports.get("good", 8443)
        tls_send(self.cfg.host, port,
                 _build("GET", "/ctl/upgrade-inject/%d" % (1 if on else 0)))

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

# Drive + Predicates
def drive(cfg, path="/ok", ep="good", rtimeout=8.0):
    raw = tls_send(cfg.host, cfg.port, _build("GET", "/call", {"X-Ep": ep, "X-Path": path}), rtimeout=rtimeout)
    if not raw or _status_of(raw) != 200:
        return None
    try:
        return json.loads(_body_of(raw))
    except Exception:
        return None

def inject(cfg, mode, body, rtimeout=8.0):
    raw = tls_send(cfg.host, cfg.port, _build("POST", "/inject", {"X-Inject": mode}, body), rtimeout=rtimeout)
    if not raw or _status_of(raw) != 200:
        return None
    try:
        return json.loads(_body_of(raw))
    except Exception:
        return None

def drive_upgrade(cfg, mode="normal", key="hello", rtimeout=15.0):
    raw = tls_send(cfg.host, cfg.port, _build("GET", "/upgrade", {"X-Mode": mode, "X-Key": key}),
                   rtimeout=rtimeout)
    if not raw or _status_of(raw) != 200:
        return None
    try:
        return json.loads(_body_of(raw))
    except Exception:
        return None

def is_ok(r, status=None, body=None):
    if not r or r.get("ep") != EP_SUCCESS:
        return False
    if status is not None and r.get("status") != status:
        return False
    if body is not None and r.get("body") != body:
        return False
    return True
def is_err(r):
    return bool(r) and r.get("ep") != EP_SUCCESS
def is_errc(r, code):
    return bool(r) and r.get("ep") == code

_sid = itertools.count(1)
def _next_sid():
    return "s%d" % next(_sid)

def drive_staged(cfg, mock, blob, ep="good", keep=True):
    sid = _next_sid()
    mock.stage(sid, blob, keep, ep=ep)
    return drive(cfg, "/raw/%s" % sid, ep=ep)

# Phases
def phase_handshake(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("handshake")
    p.check("TLS GET /ok round-trips", is_ok(drive(cfg, "/ok"), 200, "hello"), "got %r" % drive(cfg, "/ok"))
    p.check("TLS empty body",          is_ok(drive(cfg, "/empty"), 200, ""))
    p.check("TLS large body (4000)",   (lambda r: is_ok(r, 200) and r.get("bodylen") == 4000)(drive(cfg, "/cl/4000")))
    p.check("TLS chunked",             is_ok(drive(cfg, "/chunked"), 200, "abcde"))
    p.check("TLS keep-alive reuse x10", all(is_ok(drive(cfg, "/ok"), 200, "hello") for _ in range(10)))
    hs, hf, rq, _ = mock.stats("good")
    p.check("good listener completed handshakes", hs >= 1, "stats good=%r" % [hs, hf, rq])

def phase_verify(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("verify")
    labels = {
        "selfsigned": "untrusted self-signed cert refused",
        "wronghost":  "hostname-mismatch cert refused",
        "expired":    "expired cert refused",
    }
    for name, port, cert, opts, expect in PERSONAS:
        if expect != "refuse" or name == "tls12":
            continue
        if not persona_available(cfg, cert):
            p.check(labels.get(name, name) + " (skipped, no cert)", True)
            continue
        r = drive(cfg, "/ok", ep=name, rtimeout=12)
        p.check(labels[name], is_err(r),
              "client ACCEPTED it (ep=%r), MitM possible" % (r and r.get("ep")), security=True)
        hs, hf, rq, _ = mock.stats(name)
        # mock.stats() itself dials this same port with an UNVERIFIED TLS client to
        # fetch the counters, so `hs` always includes that one successful handshake
        # regardless of what the WFX client did, it is not evidence the client
        # completed a handshake. Only hs_fail speaks to the client here
        p.check(name + ": client bailed at TLS layer", hf >= 1,
              "handshakes=%d hs_fail=%d (want at least one failed handshake)" % (hs, hf), security=True)

def phase_protocol(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("protocol")
    if not persona_available(cfg, "good"):
        p.check("TLS 1.2 downgrade refused (skipped)", True)
        return
    r = drive(cfg, "/ok", ep="tls12", rtimeout=12)
    p.check("TLS 1.2 downgrade refused", is_err(r),
          "client accepted a TLS 1.2 downgrade (ep=%r)" % (r and r.get("ep")), security=True)

# PHASE: inbound mTLS (client_ca_path), the opposite direction from every other phase in this
# file - here WE are the client presenting a cert, and WFX's OWN listener is what does the
# verifying. No mock involved: every vector here dials WFX directly on cfg.port
#
# This is why tls_send() defaults to the trusted client cert (CLIENT_CERT/CLIENT_KEY above) -
# client_ca_path is on for the whole suite (see patch_ssl_paths), so every other phase's calls
# would refuse at the handshake without it. Refusal is proven the same way phase_verify proves it
# for the outbound side: the call itself returns nothing, because WFX drops the connection at the
# TLS layer before any HTTP response exists (HandleClientHandshake -> Close, no alert-visible
# response body to inspect, just a dead socket)
def phase_mtls(ctx):
    cfg = ctx.cfg
    p = ctx.phase("mtls")

    if "client-good" not in cfg.avail:
        p.check("inbound mTLS vectors (skipped, no client CA)", True)
        return

    cd = cfg.cert_dir
    def cert(name):
        return os.path.join(cd, name + ".pem"), os.path.join(cd, name + "-key.pem")

    # Baseline: the same trusted cert every other phase relies on implicitly via tls_send()'s
    # default, checked explicitly here so a broken default reads as "mtls" failing, not the
    # entire rest of the suite failing at once
    r = tls_send(cfg.host, cfg.port, _build("GET", "/health"))
    p.check("trusted client cert accepted", _status_of(r) == 200, "status=%r" % _status_of(r))

    # The crown jewel: FAIL_IF_NO_PEER_CERT is the entire point of requiring a client cert
    # Refusal here doesn't reliably surface as wrap_socket() raising (net.send() -> None): unlike
    # the outbound side (WFX validates the PEER's cert mid-handshake, so it bails before its own
    # Finished), here WFX is the one deciding, and only after the audit has already sent its own
    # Certificate/Finished - wrap_socket() on our side often returns fine, and the rejection alert
    # only surfaces on the first read afterwards, which _drain() swallows into b"" rather than
    # raising. So the real signal is "no successful HTTP response", not "send() returned None"
    r = tls_send(cfg.host, cfg.port, _build("GET", "/health"), certfile=None, keyfile=None)
    p.check("no client cert refused", _status_of(r) != 200,
          "server answered without a client cert (status=%r), mTLS is not actually required" % _status_of(r),
          security=True)

    refuse = [
        ("client-otherca",    "client cert signed by an untrusted CA refused"),
        ("client-selfsigned", "self-signed client cert refused"),
        ("client-expired",    "expired client cert refused"),
    ]
    for name, label in refuse:
        if name not in cfg.avail:
            p.check(label + " (skipped, no cert)", True)
            continue
        certfile, keyfile = cert(name)
        r = tls_send(cfg.host, cfg.port, _build("GET", "/health"), certfile=certfile, keyfile=keyfile)
        p.check(label, _status_of(r) != 200,
              "server ACCEPTED it (status=%r), inbound auth bypass possible" % _status_of(r), security=True)

    # Chain building: client_ca_path only loads the root, WFX can't fetch a missing intermediate
    # itself - a leaf alone must fail even though it ultimately chains to a trusted root
    if "client-viaint" in cfg.avail:
        leaf, leafkey = cert("client-viaint")
        r = tls_send(cfg.host, cfg.port, _build("GET", "/health"), certfile=leaf, keyfile=leafkey)
        p.check("intermediate-signed leaf without the intermediate refused", _status_of(r) != 200,
              "server accepted an unbuildable chain (status=%r)" % _status_of(r), security=True)

        chain = os.path.join(cd, "client-viaint-chain.pem")
        r = tls_send(cfg.host, cfg.port, _build("GET", "/health"), certfile=chain, keyfile=leafkey)
        p.check("intermediate-signed leaf WITH the intermediate accepted", _status_of(r) == 200,
              "status=%r" % _status_of(r))

def phase_framing(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("framing")
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
        p.check("TLS framing ok: %s" % name, is_ok(drive_staged(cfg, mock, blob), status, expect))

    bad = [
        ("bad status HTTP/2",  b"HTTP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n"),
        ("obs-fold header",    b"HTTP/1.1 200 OK\r\nX: a\r\n b\r\nContent-Length: 2\r\n\r\nhi"),
        ("dup CL differ",      b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nhi"),
        ("CL + TE",             b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\nhi"),
        ("chunk non-hex",      b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nabc\r\n0\r\n\r\n"),
    ]
    for name, blob in bad:
        sec = name in ("obs-fold header", "dup CL differ", "CL + TE")
        p.check("TLS framing bad: %s" % name, is_err(drive_staged(cfg, mock, blob)),
              "malformed framing accepted over TLS", security=sec)

    trunc = [
        ("mid status",  b"HTTP/1.1 200"),
        ("mid CL body", b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"),
        ("mid chunk",   b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab"),
    ]
    for name, blob in trunc:
        p.check("TLS eof: %s" % name, is_err(drive_staged(cfg, mock, blob, keep=False)))

def phase_desync(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("desync")
    poisons = [
        ("204 + body",       b"HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\nhello"),
        ("trailing smuggle", b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ngoodHTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nSMUGGLE"),
    ]
    for name, blob in poisons:
        drive_staged(cfg, mock, blob, keep=True)
        clean = all(is_ok(drive(cfg, "/ok"), 200, "hello") for _ in range(8))
        p.check("no TLS-pool poison: %s" % name, clean, "pooled TLS conn corrupted after %s" % name, security=True)
    r = drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ngoodHTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nSMUGGLE", keep=True)
    p.check("smuggled body not delivered", (r is None) or (r.get("body") != "SMUGGLE"), "delivered smuggled bytes: %r" % r, security=True)

def phase_inject(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("inject")
    path_vec = [
        ("CRLF path", b"/a\r\nX-Smuggle: 1"),
        ("bare LF",   b"/a\nX-Smuggle: 1"),
        ("bare CR",   b"/a\rX-Smuggle: 1"),
        ("NUL",       b"/a\x00b"),
        ("full req",  b"/a\r\nHost: evil\r\nGET /x HTTP/1.1\r\n"),
    ]
    for name, pl in path_vec:
        p.check("reject path: %s" % name, is_errc(inject(cfg, "path", pl), EP_SERIALIZE),
              "expected serialize-err, got %r" % inject(cfg, "path", pl), security=True)
    hdr_vec = [
        ("CRLF value", b"X-E: a\r\nX-Smuggle: b"),
        ("CRLF name",  b"X-E\r\nX: b"),
        ("NUL value",  b"X-E: a\x00b"),
    ]
    for name, pl in hdr_vec:
        p.check("reject header: %s" % name, is_errc(inject(cfg, "header", pl), EP_SERIALIZE),
              "expected serialize-err", security=True)
    p.check("clean path accepted",   is_ok(inject(cfg, "path", b"/ok"), 200, "hello"))
    p.check("clean header accepted", is_ok(inject(cfg, "header", b"X-Ok: fine"), 200, "hello"))

def phase_resource(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("resource")
    t0 = time.time()
    p.check("slow response -> timeout", is_err(drive(cfg, "/slow", ep="fast", rtimeout=18)), "elapsed %.1fs" % (time.time() - t0))
    r = drive(cfg, "/truncate", ep="good", rtimeout=12)
    p.check("truncation not delivered as success",
          (r is None) or (r.get("ep") != EP_SUCCESS) or (r.get("body") != "partial-body-then-brutal-reset"),
          "truncated body delivered as complete (ep=%r body=%r)" % (r and r.get("ep"), r and r.get("body")), security=True)

# WFX's outbound client caches one TLS session per configured HttpEndpoint (see
# http_openssl.cpp's NewClientSessionCallback / EndpointMetadata::cachedTlsSession) and
# offers it back on the next connection to that same endpoint. A live TCP+TLS connection
# just gets kept alive and reused by the connection pool though, so resumption only has
# a chance to fire once the pooled connection actually dies and a new one has to be
# opened - /truncate resets the connection, which is how we force that here
def phase_resumption(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("resumption")
    if not persona_available(cfg, "good"):
        p.check("TLS session resumption (skipped, no cert)", True)
        return

    # Warm-up: cache is empty, this must be a full handshake
    r = drive(cfg, "/ok", ep="good", rtimeout=12)
    p.check("warm-up call succeeds", is_ok(r, 200, "hello"), "got %r" % r)

    # Kill every pooled connection (connLimit=4 for 'good' - more than 4 resets makes
    # it very likely every pooled slot actually got cycled at least once)
    for _ in range(6):
        drive(cfg, "/truncate", ep="good", rtimeout=12)

    # Each of these must reconnect (or find an already-reconnected pool slot) and still
    # work; at least one of them should land on a fresh connection that resumes the
    # session cached by the warm-up call above
    reconnected = [drive(cfg, "/ok", ep="good", rtimeout=12) for _ in range(6)]
    p.check("calls after forced reconnect still succeed",
          all(is_ok(r, 200, "hello") for r in reconnected), "got %r" % reconnected)

    hs, hf, rq, resumed = mock.stats("good")
    p.check("session resumed at least once after reconnect", resumed >= 1,
          "stats good=%r (handshakes, hs_fail, requests, resumed)" % [hs, hf, rq, resumed])

# PHASE: in-band TLS upgrade across the trust boundary
#
# UpgradeToTLS is a generic STARTTLS primitive, so it inherits that family's CVE
# history. The two vectors here both need a REAL handshake, which is why they sit
# in this suite rather than endpoint_audit (which covers the cert-free half:
# downgrade-refusal and garbage-instead-of-handshake)
#
# The mock can append "OK 9999" as plaintext immediately after answering "S", before
# wrapping the socket. That is CVE-2011-0411 (Postfix) / CVE-2026-41319 (MailKit):
# an engine that reuses its read buffer across the upgrade hands attacker bytes to
# the protocol as though the authenticated peer had sent them. In MailKit's case
# it enabled a SASL downgrade to PLAIN
#
# Injection is toggled per run rather than left on, because with it on a handshake
# failure is a legitimate secure outcome (the injected bytes may still be sitting
# unread in the kernel buffer, where they land in front of the ServerHello and break
# the handshake), and that outcome cannot be told apart from a broken upgrade unless
# the clean path is measured separately
def phase_upgrade(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("upgrade")

    if not persona_available(cfg, "good"):
        p.check("upgrade persona available", False, "good cert missing")
        return

    # Clean: no injection, so the upgrade must work end to end
    mock.upgrade_inject(False)

    r = drive_upgrade(cfg)
    p.check("upgrade: STARTTLS handshake completes", is_ok(r), "r=%r" % r)

    p.check("upgrade: response served over TLS with the authenticated id",
            bool(r) and r.get("conn", 0) > 0
            and r.get("value", "").startswith("%d:" % (r.get("conn") or -1)),
            "value=%r conn=%r" % ((r or {}).get("value"), (r or {}).get("conn")))

    # Reuse: a second call over the upgraded, pooled connection must still work
    r2 = drive_upgrade(cfg, key="second")
    p.check("upgrade: upgraded connection reusable", is_ok(r2), "r=%r" % r2)

    # Hostile: the load-bearing assertion. 9999 is the id the injected PLAINTEXT
    # claimed, a real id comes from the mock's accept counter and starts at 1
    # Failing the call is fine, trusting the injected id is not
    mock.upgrade_inject(True)

    inj = drive_upgrade(cfg, key="injected")
    conn = (inj or {}).get("conn")
    value = (inj or {}).get("value", "")

    p.check("upgrade: pre-TLS injected plaintext never trusted after the boundary",
            conn != 9999 and not value.startswith("9999:"),
            "conn=%r value=%r (9999 means pre-upgrade bytes were trusted post-upgrade)"
            % (conn, value), security=True)

    p.check("upgrade: injected run either refuses or succeeds with a real id",
            inj is None or not is_ok(inj) or (isinstance(conn, int) and 0 < conn < 9999),
            "r=%r" % (inj,), security=True)

    mock.upgrade_inject(False)

    # Double wrap: this endpoint is tlsConfig=Require, so the engine already
    # wrapped at connect time. UpgradeToTLS must refuse rather than wrap twice
    # (which would leak the first SSL object and desync the connection)
    r3 = drive_upgrade(cfg, mode="double")
    p.check("upgrade: refused on an already-secure slot",
          bool(r3) and r3.get("ep") != EP_SUCCESS, "r=%r" % r3, security=True)

    p.check("upgrade: worker healthy after upgrade vectors",
          is_ok(drive(cfg, "/ok"), 200, "hello"), "")

class TlsAudit(common.Suite):
    name = "tls_audit"
    description = "WFX adversarial TLS client audit"
    phases = {
        "handshake":  phase_handshake,
        "verify":     phase_verify,
        "protocol":   phase_protocol,
        "mtls":       phase_mtls,
        "framing":    phase_framing,
        "desync":     phase_desync,
        "inject":     phase_inject,
        "resource":   phase_resource,
        "resumption": phase_resumption,
        "upgrade":    phase_upgrade,
    }

    def add_arguments(self, parser):
        parser.set_defaults(app_dir=os.path.join(HERE, "app"))

    def configure(self, cfg):
        cfg.cert_dir = os.path.join(HERE, "certs")
        ensure_certs(cfg)
        patch_ssl_paths(cfg)

    # WFX serves HTTPS here, so the audit speaks TLS to it for everything, /health included. And
    # since patch_ssl_paths turns client_ca_path on for the whole server, that includes the boot-
    # readiness probe itself - common.tls_probe presents no client cert, so it would be refused at
    # the handshake exactly like phase_mtls's "no cert" vector, reading as a boot crash rather than
    # what it actually is
    def build_server(self, cfg):
        def mtls_probe(probeCfg, timeout):
            raw = net.send(probeCfg.host, probeCfg.port, net.request("GET", "/health"),
                           rtimeout=timeout, ctimeout=timeout, tls=True,
                           certfile=CLIENT_CERT, keyfile=CLIENT_KEY)
            return net.status(raw) == 200

        return common.Server(cfg, flags=("--use-https", "--https-port-override"),
                             probe=mtls_probe, label="/health (HTTPS)")

    def setup(self, ctx):
        ctx.resources["mock"] = Mock(ctx.cfg)
        ctx.mock.start()

    def teardown(self, ctx):
        if "mock" in ctx.resources:
            ctx.mock.stop()

if __name__ == "__main__":
    common.run(TlsAudit)
