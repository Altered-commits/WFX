#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX crypto ABI audit
#
# Boots the crypto_audit test app and drives every route in app/src/main.cpp,
# checking WFX::Sha256/384/512, HmacSha256/384/512 (one-shot + streaming),
# AeadEncrypt/AeadDecrypt (AES-256-GCM + ChaCha20-Poly1305), Pbkdf2/Hkdf/Argon2id,
# RandomBytes, and ConstantTimeEquals
#
# Where a Python stdlib oracle exists (hashlib, hmac, hashlib.pbkdf2_hmac, a
# hand-rolled RFC 5869 HKDF-SHA256), results are checked byte-for-byte against
# it. AEAD and Argon2id have no stdlib oracle, so those are checked structurally:
# round-trip correctness, tamper detection, cross-algo rejection, and (for
# Argon2id) reproducibility
#
# Exit codes: 0 all pass, 1 any failure

import hashlib
import hmac
import json
import os
import sys

# Suites are run directly, so tests/ has to be on the path before common is importable
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import common
from common import net, term

HERE = os.path.dirname(os.path.abspath(__file__))

# CryptoStatus ints (shared/abis/crypto_types.hpp)
ST_OK = 0
ST_INVALID_ARG = 1
ST_BUFFER_TOO_SMALL = 2
ST_AUTH_FAILED = 3
ST_UNSUPPORTED = 4
ST_INTERNAL_ERROR = 5

HASH_ALGOS = {"sha256": hashlib.sha256, "sha384": hashlib.sha384, "sha512": hashlib.sha512}

# Routes answer JSON, so every call goes through here. A failure is reported rather than swallowed:
# a null result otherwise looks identical whether the route 500'd, the worker died, or the JSON was
# malformed, and that ambiguity costs real debugging time
def call(cfg, method, path, headers=None, payload=b"", rtimeout=30.0):
    raw = net.send(cfg.host, cfg.port, net.request(method, path, headers, payload), rtimeout=rtimeout)
    if not raw:
        term.log("debug", term.yellow(
            "%s %s -> no response (connection failed, reset, or timed out)" % (method, path)))
        return None

    code = net.status(raw)
    if code != 200:
        term.log("debug", term.yellow(
            "%s %s -> HTTP %r, body[:200]=%r" % (method, path, code, net.body(raw)[:200])))
        return None

    try:
        return json.loads(net.body(raw))
    except ValueError as exc:
        term.log("debug", term.yellow(
            "%s %s -> 200 but bad JSON: %r, body[:200]=%r" % (method, path, exc, net.body(raw)[:200])))
        return None

def hash_call(cfg, algo, data, path="/crypto/hash"):
    return call(cfg, "POST", path, {"X-Algo": algo}, data)

def hmac_call(cfg, algo, key, data, path="/crypto/hmac"):
    return call(cfg, "POST", path, {"X-Algo": algo, "X-Key": key.hex()}, data)

def aead_call(cfg, path, algo, key, nonce, aad, data, rtimeout=30.0):
    headers = {"X-Algo": algo, "X-Key": key.hex(), "X-Nonce": nonce.hex(), "X-Aad": aad.hex()}
    return call(cfg, "POST", path, headers, data, rtimeout=rtimeout)

def kdf_call(cfg, path, headers):
    return call(cfg, "GET", path, headers)

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
def phase_hash(ctx):
    cfg = ctx.cfg
    p = ctx.phase("hash")
    vectors = [b"", b"hello world", b"The quick brown fox jumps over the lazy dog", os.urandom(1000), b"A" * 100000]

    for algo, hasher in HASH_ALGOS.items():
        for v in vectors:
            expect = hasher(v).hexdigest()
            r = hash_call(cfg, algo, v)
            p.check("hash %s len=%d" % (algo, len(v)),
                  bool(r) and r.get("status") == ST_OK and r.get("hex") == expect,
                  "got %r want %s" % (r, expect))

            r2 = hash_call(cfg, algo, v, path="/crypto/hash-stream")
            p.check("hash-stream %s len=%d" % (algo, len(v)),
                  bool(r2) and r2.get("status") == ST_OK and r2.get("hex") == expect,
                  "got %r want %s" % (r2, expect))

# PHASE: response body actually streamed via res.Stream() + HashStream fed per-callback
def phase_stream_response(ctx):
    cfg = ctx.cfg
    p = ctx.phase("stream-response")
    cases = [(20, 64), (1, 1), (5, 1000), (0, 64), (3, 1)]

    for algo, hasher in HASH_ALGOS.items():
        for num_chunks, chunk_len in cases:
            expect_content = b"".join(
                bytes([ord('A') + (c % 26)]) * chunk_len for c in range(num_chunks))
            expect_digest = hasher(expect_content).hexdigest()

            raw = net.send(cfg.host, cfg.port,
                           net.request("GET", "/crypto/hash-stream-response",
                                  {"X-Algo": algo, "X-Chunks": str(num_chunks), "X-Chunklen": str(chunk_len)}),
                           rtimeout=15.0)
            name = "stream-response %s chunks=%d len=%d" % (algo, num_chunks, chunk_len)
            if not raw or net.status(raw) != 200:
                p.check(name, False, "no/bad response: status=%r" % (net.status(raw) if raw else None))
                continue

            body = net.dechunk(net.body(raw))
            marker = b"\n#DIGEST:"
            idx = body.find(marker)
            if idx < 0:
                p.check(name, False, "no digest footer found in %d-byte body" % len(body))
                continue

            content, footer = body[:idx], body[idx + len(marker):]
            digest_hex = footer.strip().decode("ascii", "replace")

            ok = (content == expect_content) and (digest_hex == expect_digest)
            p.check(name, ok, "content match=%s digest got=%s want=%s" %
                  (content == expect_content, digest_hex, expect_digest))

# PHASE: one-shot + streaming HMAC
def phase_hmac(ctx):
    cfg = ctx.cfg
    p = ctx.phase("hmac")
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
            p.check("hmac %s keylen=%d datalen=%d" % (algo, len(key), len(data)),
                  bool(r) and r.get("status") == ST_OK and r.get("hex") == expect,
                  "got %r want %s" % (r, expect))

            r2 = hmac_call(cfg, algo, key, data, path="/crypto/hmac-stream")
            p.check("hmac-stream %s keylen=%d datalen=%d" % (algo, len(key), len(data)),
                  bool(r2) and r2.get("status") == ST_OK and r2.get("hex") == expect,
                  "got %r want %s" % (r2, expect))

    # RFC 4231 test case 1 exact vector (key=0x0b*20, data="Hi There")
    expect_sha256 = "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
    r = hmac_call(cfg, "sha256", b"\x0b" * 20, b"Hi There")
    p.check("hmac sha256 RFC4231 case1", bool(r) and r.get("hex") == expect_sha256,
          "got %r want %s" % (r, expect_sha256))

# PHASE: AEAD (AES-256-GCM + ChaCha20-Poly1305)
def phase_aead(ctx):
    cfg = ctx.cfg
    p = ctx.phase("aead")
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
            p.check("%s encrypt len=%d" % (algo, pt_len), enc_ok, "got %r" % enc)
            if not enc_ok:
                continue

            ct = bytes.fromhex(enc["hex"])
            dec = aead_call(cfg, "/crypto/aead/decrypt", algo, key, nonce, aad, ct)
            dec_ok = bool(dec) and dec.get("status") == ST_OK and dec.get("hex", "") == pt.hex()
            p.check("%s round-trip len=%d" % (algo, pt_len), dec_ok, "got %r want %s" % (dec, pt.hex()))

            if pt_len > 0:
                tampered = bytearray(ct)
                tampered[0] ^= 0x01
                dec_bad = aead_call(cfg, "/crypto/aead/decrypt", algo, key, nonce, aad, bytes(tampered))
                p.check("%s tamper detected len=%d" % (algo, pt_len),
                      bool(dec_bad) and dec_bad.get("status") == ST_AUTH_FAILED,
                      "got %r" % dec_bad, security=True)

            wrong_aad = aead_call(cfg, "/crypto/aead/decrypt", algo, key, nonce, os.urandom(8), ct)
            p.check("%s wrong aad rejected len=%d" % (algo, pt_len),
                  bool(wrong_aad) and wrong_aad.get("status") == ST_AUTH_FAILED,
                  "got %r" % wrong_aad, security=True)

    # Cross-algo: ciphertext produced under one algo must not decrypt under the other
    key, nonce, aad, pt = os.urandom(32), os.urandom(12), os.urandom(4), b"cross-algo-check"
    enc = aead_call(cfg, "/crypto/aead/encrypt", "aesgcm", key, nonce, aad, pt)
    if enc and enc.get("status") == ST_OK:
        ct = bytes.fromhex(enc["hex"])
        cross = aead_call(cfg, "/crypto/aead/decrypt", "chacha", key, nonce, aad, ct)
        p.check("cross-algo decrypt rejected",
              bool(cross) and cross.get("status") == ST_AUTH_FAILED, "got %r" % cross, security=True)

    # Wrong-length key/nonce -> INVALID_ARG, not a crash
    bad_key = aead_call(cfg, "/crypto/aead/encrypt", "aesgcm", os.urandom(16), os.urandom(12), b"", b"x")
    p.check("short key -> invalid_arg", bool(bad_key) and bad_key.get("status") == ST_INVALID_ARG, "got %r" % bad_key)
    bad_nonce = aead_call(cfg, "/crypto/aead/encrypt", "aesgcm", os.urandom(32), os.urandom(8), b"", b"x")
    p.check("short nonce -> invalid_arg", bool(bad_nonce) and bad_nonce.get("status") == ST_INVALID_ARG, "got %r" % bad_nonce)

# PHASE: AEAD input-size cap (oversized plaintext rejected, not OOM'd)
def phase_aead_cap(ctx):
    cfg = ctx.cfg
    p = ctx.phase("aead-cap")
    key, nonce, aad = os.urandom(32), os.urandom(12), b""
    over_cap = b"\x00" * (64 * 1024 * 1024 + 1)
    r = aead_call(cfg, "/crypto/aead/encrypt", "aesgcm", key, nonce, aad, over_cap, rtimeout=60.0)
    p.check("64MiB+1 rejected (INVALID_ARG)", bool(r) and r.get("status") == ST_INVALID_ARG, "got %r" % r)

# PHASE: key derivation
def phase_kdf(ctx):
    cfg = ctx.cfg
    p = ctx.phase("kdf")

    for password, salt, iters, out_len in [
        (b"password", b"salt", 1000, 32),
        (b"", b"", 1, 16),
        (os.urandom(16), os.urandom(16), 2000, 64),
    ]:
        expect = hashlib.pbkdf2_hmac("sha256", password, salt, iters, out_len).hex()
        r = kdf_call(cfg, "/crypto/pbkdf2", {
            "X-Password": password.hex(), "X-Salt": salt.hex(), "X-Iter": str(iters), "X-Outlen": str(out_len)})
        p.check("pbkdf2 iters=%d outlen=%d" % (iters, out_len),
              bool(r) and r.get("status") == ST_OK and r.get("hex") == expect, "got %r want %s" % (r, expect))

    for ikm, salt, info, out_len in [
        (b"input key material", b"salt value", b"context info", 32),
        (os.urandom(32), b"", b"", 42),
        (os.urandom(16), os.urandom(16), os.urandom(8), 64),
    ]:
        expect = hkdf_sha256(ikm, salt, info, out_len).hex()
        r = kdf_call(cfg, "/crypto/hkdf", {
            "X-Ikm": ikm.hex(), "X-Salt": salt.hex(), "X-Info": info.hex(), "X-Outlen": str(out_len)})
        p.check("hkdf outlen=%d" % out_len,
              bool(r) and r.get("status") == ST_OK and r.get("hex") == expect, "got %r want %s" % (r, expect))

    # Argon2id: no stdlib oracle, check structurally instead
    pw, salt = b"correct horse battery staple", os.urandom(16)
    hdrs = {"X-Password": pw.hex(), "X-Salt": salt.hex(), "X-Iter": "2", "X-Memkb": "8192",
            "X-Parallelism": "1", "X-Outlen": "32"}
    r1 = kdf_call(cfg, "/crypto/argon2id", hdrs)
    r2 = kdf_call(cfg, "/crypto/argon2id", hdrs)
    p.check("argon2id deterministic (same inputs)",
          bool(r1) and bool(r2) and r1.get("status") == ST_OK and r1.get("hex") == r2.get("hex"),
          "r1=%r r2=%r" % (r1, r2))
    p.check("argon2id output length correct",
          bool(r1) and len(r1.get("hex", "")) == 64, "got %r" % r1)  # 32 bytes -> 64 hex chars

    hdrs_diff_salt = dict(hdrs, **{"X-Salt": os.urandom(16).hex()})
    r3 = kdf_call(cfg, "/crypto/argon2id", hdrs_diff_salt)
    p.check("argon2id different salt -> different output",
          bool(r3) and r3.get("status") == ST_OK and r3.get("hex") != r1.get("hex"),
          "r1=%r r3=%r" % (r1, r3))

# PHASE: misc (RandomBytes, ConstantTimeEquals)
def phase_misc(ctx):
    cfg = ctx.cfg
    p = ctx.phase("misc")

    # len=0 is a no-op success (nothing to compute), not an error
    r0 = kdf_call(cfg, "/crypto/random", {"X-Len": "0"})
    p.check("random len=0 -> ok, empty", bool(r0) and r0.get("status") == ST_OK and r0.get("hex") == "",
          "got %r" % r0)

    for n in (1, 16, 32, 4096):
        r = kdf_call(cfg, "/crypto/random", {"X-Len": str(n)})
        p.check("random len=%d" % n, bool(r) and r.get("status") == ST_OK and len(r.get("hex", "")) == n * 2,
              "got %r" % r)

    r1 = kdf_call(cfg, "/crypto/random", {"X-Len": "64"})
    r2 = kdf_call(cfg, "/crypto/random", {"X-Len": "64"})
    p.check("random bytes differ across calls",
          bool(r1) and bool(r2) and r1.get("hex") != r2.get("hex"), "r1=%r r2=%r" % (r1, r2))
    p.check("random bytes not all-zero",
          bool(r1) and r1.get("hex") != "00" * 64, "got %r" % r1)

    a = os.urandom(32)
    r = kdf_call(cfg, "/crypto/consttime", {"X-A": a.hex(), "X-B": a.hex()})
    p.check("consttime equal", bool(r) and r.get("equal") is True, "got %r" % r)

    b2 = bytearray(a)
    b2[0] ^= 0x01
    r = kdf_call(cfg, "/crypto/consttime", {"X-A": a.hex(), "X-B": bytes(b2).hex()})
    p.check("consttime unequal (1 byte)", bool(r) and r.get("equal") is False, "got %r" % r)

    r = kdf_call(cfg, "/crypto/consttime", {"X-A": a.hex(), "X-B": (a + b"\x00").hex()})
    p.check("consttime unequal (length)", bool(r) and r.get("equal") is False, "got %r" % r)

    # Robustness: the app decodes hex-encoded headers (key/nonce/salt/...). Odd-length
    # and non-hex inputs hit FromHex's failure path, which must be handled gracefully,
    # never crash the worker. No oracle here, the assertion is survival plus a
    # well-formed response, with the sanitizer gate catching any memory fault
    for bad_hex in ("0", "zzz", "gg", "12345", "x" * 129, " ", "%41"):
        r = hmac_call(cfg, "sha256", b"", b"data")  # baseline shape
        rr = call(cfg, "POST", "/crypto/hmac", {"X-Algo": "sha256", "X-Key": bad_hex}, b"data")
        p.check("malformed key hex survives: %r" % bad_hex,
              rr is not None and "status" in rr, "got %r" % rr)

    for bad_hex in ("1", "zz", "g0"):
        rr = call(cfg, "POST", "/crypto/aead/encrypt",
                  {"X-Algo": "aesgcm", "X-Key": bad_hex, "X-Nonce": bad_hex, "X-Aad": ""}, b"pt")
        p.check("malformed aead hex survives: %r" % bad_hex,
              rr is not None and "status" in rr, "got %r" % rr)

class CryptoAudit(common.Suite):
    name = "crypto_audit"
    description = "WFX crypto ABI audit"
    phases = {
        "hash":            phase_hash,
        "stream-response": phase_stream_response,
        "hmac":            phase_hmac,
        "aead":            phase_aead,
        "aead-cap":        phase_aead_cap,
        "kdf":             phase_kdf,
        "misc":            phase_misc,
    }

if __name__ == "__main__":
    common.run(CryptoAudit)
