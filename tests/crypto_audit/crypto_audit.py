#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX crypto ABI audit harness.
#
# Boots the crypto_audit test app and drives every route in app/src/main.cpp,
# checking WFX::Sha256/384/512, HmacSha256/384/512 (one-shot + streaming),
# AeadEncrypt/AeadDecrypt (AES-256-GCM + ChaCha20-Poly1305), Pbkdf2/Hkdf/Argon2id,
# RandomBytes, and ConstantTimeEquals.
#
# Where a Python stdlib oracle exists (hashlib, hmac, hashlib.pbkdf2_hmac, a
# hand-rolled RFC 5869 HKDF-SHA256), results are checked byte-for-byte against
# it. AEAD and Argon2id have no stdlib oracle, so those are checked structurally:
# round-trip correctness, tamper detection, cross-algo rejection, and (for
# Argon2id) reproducibility.
#
# Exit codes: 0 all pass, 1 any failure.

import argparse
import hashlib
import hmac
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import _audit_common as common

_green, _red, _yellow, _cyan, _bold = common.green, common.red, common.yellow, common.cyan, common.bold
_log, _hdr = common.log, common.hdr
raw_send = common.raw_send
_build = common.build_request
_body_of = common.response_body
_status_of = common.response_status
LogFollower = common.LogFollower
Results = common.Results
check = common.check

HERE = os.path.dirname(os.path.abspath(__file__))

class Cfg: pass

# CryptoStatus ints (shared/abis/crypto_types.hpp)
ST_OK = 0
ST_INVALID_ARG = 1
ST_BUFFER_TOO_SMALL = 2
ST_AUTH_FAILED = 3
ST_UNSUPPORTED = 4
ST_INTERNAL_ERROR = 5

HASH_ALGOS = {"sha256": hashlib.sha256, "sha384": hashlib.sha384, "sha512": hashlib.sha512}

# Server
class Server:
    def __init__(self, cfg):
        self.cfg = cfg
        self._up = False

    def start(self):
        cmd = [self.cfg.wfx, "run", self.cfg.app_dir, "--port", str(self.cfg.port), "--detach"]
        _log("server", "starting: %s" % " ".join(cmd))
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("wfx run failed (rc=%d): %s %s" % (r.returncode, r.stdout.strip(), r.stderr.strip()))
        self._up = True
        _log("server", _green("detached OK"))

    def wait_ready(self):
        _log("server", "waiting for /health …")
        t0 = time.time()
        while time.time() - t0 < self.cfg.ready_timeout:
            raw = raw_send(self.cfg.host, self.cfg.port, _build("GET", "/health"), rtimeout=1.0, ctimeout=1.0)
            if raw and _status_of(raw) == 200:
                _log("server", _green("up in %.1fs" % (time.time() - t0)))
                return True
            time.sleep(0.3)
        return False

    def alive(self):
        raw = raw_send(self.cfg.host, self.cfg.port, _build("GET", "/health"), rtimeout=2.0, ctimeout=2.0)
        return bool(raw) and _status_of(raw) == 200

    def stop(self):
        if not self._up:
            return
        _log("server", "stopping …")
        subprocess.run([self.cfg.wfx, "control", "stop", "app"], capture_output=True, text=True)

# HTTP helpers
def _get_json(cfg, method, path, headers=None, body=b"", rtimeout=30.0):
    raw = raw_send(cfg.host, cfg.port, _build(method, path, headers, body), rtimeout=rtimeout)
    if not raw:
        _log("debug", _yellow("%s %s -> no response at all (connection failed/reset/timed out)" % (method, path)))
        return None
    status = _status_of(raw)
    if status != 200:
        snippet = _body_of(raw)[:200]
        _log("debug", _yellow("%s %s -> HTTP %r (not 200), body[:200]=%r" % (method, path, status, snippet)))
        return None
    try:
        import json
        return json.loads(_body_of(raw))
    except Exception as e:
        _log("debug", _yellow("%s %s -> HTTP 200 but JSON parse failed: %r, body[:200]=%r" %
                              (method, path, e, _body_of(raw)[:200])))
        return None

def dechunk(raw_body):
    """Minimal chunked-transfer-encoding decoder (stdlib-only, no http.client)."""
    out = bytearray()
    i = 0
    while i < len(raw_body):
        j = raw_body.find(b"\r\n", i)
        if j < 0:
            break
        size_line = raw_body[i:j].split(b";")[0].strip()
        try:
            size = int(size_line, 16)
        except ValueError:
            break
        if size == 0:
            break
        start = j + 2
        out += raw_body[start:start + size]
        i = start + size + 2
    return bytes(out)

def hash_call(cfg, algo, data, path="/crypto/hash"):
    r = _get_json(cfg, "POST", path, {"X-Algo": algo}, data)
    return r

def hmac_call(cfg, algo, key, data, path="/crypto/hmac"):
    r = _get_json(cfg, "POST", path, {"X-Algo": algo, "X-Key": key.hex()}, data)
    return r

def aead_call(cfg, path, algo, key, nonce, aad, data, rtimeout=30.0):
    headers = {"X-Algo": algo, "X-Key": key.hex(), "X-Nonce": nonce.hex(), "X-Aad": aad.hex()}
    return _get_json(cfg, "POST", path, headers, data, rtimeout=rtimeout)

def kdf_call(cfg, path, headers):
    return _get_json(cfg, "GET", path, headers)

def hex_of(b):
    return b.hex() if isinstance(b, (bytes, bytearray)) else b

# RFC 5869 HKDF-SHA256, hand-rolled oracle (Python stdlib has no HKDF)
def hkdf_sha256(ikm, salt, info, length):
    if not salt:
        salt = b"\x00" * 32
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    t = b""
    okm = b""
    counter = 1
    while len(okm) < length:
        t = hmac.new(prk, t + info + bytes([counter]), hashlib.sha256).digest()
        okm += t
        counter += 1
    return okm[:length]

# PHASE: one-shot + streaming hashing
def phase_hash(cfg, results):
    b = results.phase("hash")
    vectors = [b"", b"hello world", b"The quick brown fox jumps over the lazy dog", os.urandom(1000), b"A" * 100000]

    for algo, hasher in HASH_ALGOS.items():
        for v in vectors:
            expect = hasher(v).hexdigest()
            r = hash_call(cfg, algo, v)
            check(b, "hash %s len=%d" % (algo, len(v)),
                  bool(r) and r.get("status") == ST_OK and r.get("hex") == expect,
                  "got %r want %s" % (r, expect))

            r2 = hash_call(cfg, algo, v, path="/crypto/hash-stream")
            check(b, "hash-stream %s len=%d" % (algo, len(v)),
                  bool(r2) and r2.get("status") == ST_OK and r2.get("hex") == expect,
                  "got %r want %s" % (r2, expect))

# PHASE: response body actually streamed via res.Stream() + HashStream fed per-callback
def phase_stream_response(cfg, results):
    b = results.phase("stream-response")
    cases = [(20, 64), (1, 1), (5, 1000), (0, 64), (3, 1)]

    for algo, hasher in HASH_ALGOS.items():
        for num_chunks, chunk_len in cases:
            expect_content = b"".join(
                bytes([ord('A') + (c % 26)]) * chunk_len for c in range(num_chunks))
            expect_digest = hasher(expect_content).hexdigest()

            raw = raw_send(cfg.host, cfg.port,
                           _build("GET", "/crypto/hash-stream-response",
                                  {"X-Algo": algo, "X-Chunks": str(num_chunks), "X-Chunklen": str(chunk_len)}),
                           rtimeout=15.0)
            name = "stream-response %s chunks=%d len=%d" % (algo, num_chunks, chunk_len)
            if not raw or _status_of(raw) != 200:
                check(b, name, False, "no/bad response: status=%r" % (_status_of(raw) if raw else None))
                continue

            body = dechunk(_body_of(raw))
            marker = b"\n#DIGEST:"
            idx = body.find(marker)
            if idx < 0:
                check(b, name, False, "no digest footer found in %d-byte body" % len(body))
                continue

            content, footer = body[:idx], body[idx + len(marker):]
            digest_hex = footer.strip().decode("ascii", "replace")

            ok = (content == expect_content) and (digest_hex == expect_digest)
            check(b, name, ok, "content match=%s digest got=%s want=%s" %
                  (content == expect_content, digest_hex, expect_digest))

# PHASE: one-shot + streaming HMAC
def phase_hmac(cfg, results):
    b = results.phase("hmac")
    vectors = [
        (b"", b""),
        (b"key", b"The quick brown fox jumps over the lazy dog"),
        (os.urandom(16), b""),
        (os.urandom(32), os.urandom(2000)),
        (b"\x0b" * 20, b"Hi There"),  # RFC 4231 case 1 shape (SHA-256 vector)
    ]

    for algo, hasher in HASH_ALGOS.items():
        for key, data in vectors:
            expect = hmac.new(key, data, hasher).hexdigest()
            r = hmac_call(cfg, algo, key, data)
            check(b, "hmac %s keylen=%d datalen=%d" % (algo, len(key), len(data)),
                  bool(r) and r.get("status") == ST_OK and r.get("hex") == expect,
                  "got %r want %s" % (r, expect))

            r2 = hmac_call(cfg, algo, key, data, path="/crypto/hmac-stream")
            check(b, "hmac-stream %s keylen=%d datalen=%d" % (algo, len(key), len(data)),
                  bool(r2) and r2.get("status") == ST_OK and r2.get("hex") == expect,
                  "got %r want %s" % (r2, expect))

    # RFC 4231 test case 1 exact vector (key=0x0b*20, data="Hi There")
    expect_sha256 = "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
    r = hmac_call(cfg, "sha256", b"\x0b" * 20, b"Hi There")
    check(b, "hmac sha256 RFC4231 case1", bool(r) and r.get("hex") == expect_sha256,
          "got %r want %s" % (r, expect_sha256))

# PHASE: AEAD (AES-256-GCM + ChaCha20-Poly1305)
def phase_aead(cfg, results):
    b = results.phase("aead")
    algos = ["aesgcm", "chacha"]
    key_len, nonce_len = 32, 12

    for algo in algos:
        for pt_len in (0, 1, 16, 1000, 100000):
            key = os.urandom(key_len)
            nonce = os.urandom(nonce_len)
            aad = os.urandom(8)
            pt = os.urandom(pt_len)

            enc = aead_call(cfg, "/crypto/aead/encrypt", algo, key, nonce, aad, pt)
            enc_ok = bool(enc) and enc.get("status") == ST_OK and len(enc.get("hex", "")) == (pt_len + 16) * 2
            check(b, "%s encrypt len=%d" % (algo, pt_len), enc_ok, "got %r" % enc)
            if not enc_ok:
                continue

            ct = bytes.fromhex(enc["hex"])
            dec = aead_call(cfg, "/crypto/aead/decrypt", algo, key, nonce, aad, ct)
            dec_ok = bool(dec) and dec.get("status") == ST_OK and dec.get("hex", "") == pt.hex()
            check(b, "%s round-trip len=%d" % (algo, pt_len), dec_ok, "got %r want %s" % (dec, pt.hex()))

            if pt_len > 0:
                tampered = bytearray(ct)
                tampered[0] ^= 0x01
                dec_bad = aead_call(cfg, "/crypto/aead/decrypt", algo, key, nonce, aad, bytes(tampered))
                check(b, "%s tamper detected len=%d" % (algo, pt_len),
                      bool(dec_bad) and dec_bad.get("status") == ST_AUTH_FAILED,
                      "got %r" % dec_bad, security=True)

            wrong_aad = aead_call(cfg, "/crypto/aead/decrypt", algo, key, nonce, os.urandom(8), ct)
            check(b, "%s wrong aad rejected len=%d" % (algo, pt_len),
                  bool(wrong_aad) and wrong_aad.get("status") == ST_AUTH_FAILED,
                  "got %r" % wrong_aad, security=True)

    # Cross-algo: ciphertext produced under one algo must not decrypt under the other.
    key, nonce, aad, pt = os.urandom(32), os.urandom(12), os.urandom(4), b"cross-algo-check"
    enc = aead_call(cfg, "/crypto/aead/encrypt", "aesgcm", key, nonce, aad, pt)
    if enc and enc.get("status") == ST_OK:
        ct = bytes.fromhex(enc["hex"])
        cross = aead_call(cfg, "/crypto/aead/decrypt", "chacha", key, nonce, aad, ct)
        check(b, "cross-algo decrypt rejected",
              bool(cross) and cross.get("status") == ST_AUTH_FAILED, "got %r" % cross, security=True)

    # Wrong-length key/nonce -> INVALID_ARG, not a crash.
    bad_key = aead_call(cfg, "/crypto/aead/encrypt", "aesgcm", os.urandom(16), os.urandom(12), b"", b"x")
    check(b, "short key -> invalid_arg", bool(bad_key) and bad_key.get("status") == ST_INVALID_ARG, "got %r" % bad_key)
    bad_nonce = aead_call(cfg, "/crypto/aead/encrypt", "aesgcm", os.urandom(32), os.urandom(8), b"", b"x")
    check(b, "short nonce -> invalid_arg", bool(bad_nonce) and bad_nonce.get("status") == ST_INVALID_ARG, "got %r" % bad_nonce)

def phase_aead_cap(cfg, results):
    b = results.phase("aead-cap")
    key, nonce, aad = os.urandom(32), os.urandom(12), b""
    over_cap = b"\x00" * (64 * 1024 * 1024 + 1)
    r = aead_call(cfg, "/crypto/aead/encrypt", "aesgcm", key, nonce, aad, over_cap, rtimeout=60.0)
    check(b, "64MiB+1 rejected (INVALID_ARG)", bool(r) and r.get("status") == ST_INVALID_ARG, "got %r" % r)

# PHASE: key derivation
def phase_kdf(cfg, results):
    b = results.phase("kdf")

    for password, salt, iters, out_len in [
        (b"password", b"salt", 1000, 32),
        (b"", b"", 1, 16),
        (os.urandom(16), os.urandom(16), 2000, 64),
    ]:
        expect = hashlib.pbkdf2_hmac("sha256", password, salt, iters, out_len).hex()
        r = kdf_call(cfg, "/crypto/pbkdf2", {
            "X-Password": password.hex(), "X-Salt": salt.hex(), "X-Iter": str(iters), "X-Outlen": str(out_len)})
        check(b, "pbkdf2 iters=%d outlen=%d" % (iters, out_len),
              bool(r) and r.get("status") == ST_OK and r.get("hex") == expect, "got %r want %s" % (r, expect))

    for ikm, salt, info, out_len in [
        (b"input key material", b"salt value", b"context info", 32),
        (os.urandom(32), b"", b"", 42),
        (os.urandom(16), os.urandom(16), os.urandom(8), 64),
    ]:
        expect = hkdf_sha256(ikm, salt, info, out_len).hex()
        r = kdf_call(cfg, "/crypto/hkdf", {
            "X-Ikm": ikm.hex(), "X-Salt": salt.hex(), "X-Info": info.hex(), "X-Outlen": str(out_len)})
        check(b, "hkdf outlen=%d" % out_len,
              bool(r) and r.get("status") == ST_OK and r.get("hex") == expect, "got %r want %s" % (r, expect))

    # Argon2id: no stdlib oracle, check structurally instead.
    pw, salt = b"correct horse battery staple", os.urandom(16)
    hdrs = {"X-Password": pw.hex(), "X-Salt": salt.hex(), "X-Iter": "2", "X-Memkb": "8192",
            "X-Parallelism": "1", "X-Outlen": "32"}
    r1 = kdf_call(cfg, "/crypto/argon2id", hdrs)
    r2 = kdf_call(cfg, "/crypto/argon2id", hdrs)
    check(b, "argon2id deterministic (same inputs)",
          bool(r1) and bool(r2) and r1.get("status") == ST_OK and r1.get("hex") == r2.get("hex"),
          "r1=%r r2=%r" % (r1, r2))
    check(b, "argon2id output length correct",
          bool(r1) and len(r1.get("hex", "")) == 64, "got %r" % r1)  # 32 bytes -> 64 hex chars

    hdrs_diff_salt = dict(hdrs, **{"X-Salt": os.urandom(16).hex()})
    r3 = kdf_call(cfg, "/crypto/argon2id", hdrs_diff_salt)
    check(b, "argon2id different salt -> different output",
          bool(r3) and r3.get("status") == ST_OK and r3.get("hex") != r1.get("hex"),
          "r1=%r r3=%r" % (r1, r3))

# PHASE: misc (RandomBytes, ConstantTimeEquals)
def phase_misc(cfg, results):
    b = results.phase("misc")

    # len=0 is a no-op success (nothing to compute), not an error
    r0 = kdf_call(cfg, "/crypto/random", {"X-Len": "0"})
    check(b, "random len=0 -> ok, empty", bool(r0) and r0.get("status") == ST_OK and r0.get("hex") == "",
          "got %r" % r0)

    for n in (1, 16, 32, 4096):
        r = kdf_call(cfg, "/crypto/random", {"X-Len": str(n)})
        check(b, "random len=%d" % n, bool(r) and r.get("status") == ST_OK and len(r.get("hex", "")) == n * 2,
              "got %r" % r)

    r1 = kdf_call(cfg, "/crypto/random", {"X-Len": "64"})
    r2 = kdf_call(cfg, "/crypto/random", {"X-Len": "64"})
    check(b, "random bytes differ across calls",
          bool(r1) and bool(r2) and r1.get("hex") != r2.get("hex"), "r1=%r r2=%r" % (r1, r2))
    check(b, "random bytes not all-zero",
          bool(r1) and r1.get("hex") != "00" * 64, "got %r" % r1)

    a = os.urandom(32)
    r = kdf_call(cfg, "/crypto/consttime", {"X-A": a.hex(), "X-B": a.hex()})
    check(b, "consttime equal", bool(r) and r.get("equal") is True, "got %r" % r)

    b2 = bytearray(a); b2[0] ^= 0x01
    r = kdf_call(cfg, "/crypto/consttime", {"X-A": a.hex(), "X-B": bytes(b2).hex()})
    check(b, "consttime unequal (1 byte)", bool(r) and r.get("equal") is False, "got %r" % r)

    r = kdf_call(cfg, "/crypto/consttime", {"X-A": a.hex(), "X-B": (a + b"\x00").hex()})
    check(b, "consttime unequal (length)", bool(r) and r.get("equal") is False, "got %r" % r)

PHASES = {"hash": phase_hash, "stream-response": phase_stream_response, "hmac": phase_hmac,
          "aead": phase_aead, "aead-cap": phase_aead_cap, "kdf": phase_kdf, "misc": phase_misc}

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
    if not booted:
        return 3
    if sec or fail:
        return 1
    return 0

def main():
    ap = argparse.ArgumentParser(description="WFX crypto ABI audit")
    common.add_common_args(ap, PHASES)
    args = ap.parse_args()

    if args.ci:
        common.enable_ci_mode()

    if args.list_phases:
        for p in PHASES:
            print(p)
        return 0

    cfg = Cfg()
    cfg.host = args.host
    cfg.port = args.port
    cfg.wfx = args.wfx
    cfg.app_dir = args.app_dir
    cfg.ready_timeout = args.ready_timeout

    results = Results()
    server = Server(cfg)
    follower = LogFollower(cfg.app_dir, mode=args.wfx_logs)
    booted = False
    try:
        server.start()
        # Start tailing WFX logs immediately so a crash during boot is visible.
        follower.start()
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
            try:
                PHASES[name](cfg, results)
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
        follower.stop()
        server.stop()

if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
