#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX crypto ABI audit
#
# Boots the crypto_audit test app and drives every route in app/src/main.cpp,
# checking WFX::Sha256/384/512, HmacSha256/384/512 (one-shot + streaming),
# AeadEncrypt/AeadDecrypt (AES-256-GCM + ChaCha20-Poly1305), Pbkdf2/Hkdf/Argon2id,
# RandomBytes, ConstantTimeEquals, the asymmetric sign/verify ABI, Base64/Hex/Url
# encoding, and JWK loading
#
# Where a Python stdlib oracle exists (hashlib, hmac, hashlib.pbkdf2_hmac, a
# hand-rolled RFC 5869 HKDF-SHA256), results are checked byte-for-byte against
# it. AEAD and Argon2id have no stdlib oracle, so those are checked structurally:
# round-trip correctness, tamper detection, cross-algo rejection, and (for
# Argon2id) reproducibility. The asymmetric phases cross-check against the
# `cryptography` package (an independent implementation, not WFX's own OpenSSL
# backend) in both directions, plus a curated set of real third-party test
# vectors (Project Wycheproof's ed25519_test.json) for known-answer coverage a
# self-consistent round-trip could never catch
#
# Exit codes: 0 all pass, 1 any failure

import hashlib
import hmac
import json
import os
import sys
import time

# Suites are run directly, so tests/ has to be on the path before common is importable
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import common
from common import net, term

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, ed25519, padding, rsa, utils as asym_utils

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

########################################################################################
# vvv Asymmetric / JWK / Encoding helpers vvv
########################################################################################

def asym_call(cfg, path, headers=None, body=b"", rtimeout=30.0):
    return call(cfg, "POST", path, headers or {}, body, rtimeout=rtimeout)

def wfx_keygen(cfg, type_, bits=2048):
    return asym_call(cfg, "/crypto/asym/keygen", {"X-Type": type_, "X-Bits": str(bits)})

def wfx_sign(cfg, priv_pem_bytes, scheme, msg):
    return asym_call(cfg, "/crypto/asym/sign", {"X-Key": priv_pem_bytes.hex(), "X-Scheme": scheme}, msg)

def wfx_verify(cfg, pub_pem_bytes, scheme, msg, sig):
    sig_hex = sig.hex() if isinstance(sig, (bytes, bytearray)) else sig
    return asym_call(cfg, "/crypto/asym/verify",
                      {"X-Key": pub_pem_bytes.hex(), "X-Scheme": scheme, "X-Sig": sig_hex}, msg)

def wfx_export(cfg, key_pem_bytes, key_is_private, export_private):
    return asym_call(cfg, "/crypto/asym/export",
                      {"X-Key": key_pem_bytes.hex(), "X-KeyIsPrivate": "1" if key_is_private else "0",
                       "X-ExportPrivate": "1" if export_private else "0"})

def wfx_from_rsa_public(cfg, n_bytes, e_bytes, rtimeout=30.0):
    return asym_call(cfg, "/crypto/asym/from-rsa-public", {"X-E": e_bytes.hex()}, n_bytes, rtimeout=rtimeout)

def wfx_from_ec_public(cfg, curve, x_bytes, y_bytes):
    return asym_call(cfg, "/crypto/asym/from-ec-public",
                      {"X-Curve": curve, "X-X": x_bytes.hex(), "X-Y": y_bytes.hex()})

def wfx_jwk_load(cfg, jwks_bytes, kid, rtimeout=30.0):
    return asym_call(cfg, "/jwk/load", {"X-Kid": kid}, jwks_bytes, rtimeout=rtimeout)

def hexfield(r, field="hex"):
    h = r.get(field, "") if r else ""
    return bytes.fromhex(h) if h else b""

def priv_pem(key):
    return key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                              serialization.NoEncryption())

def pub_pem(key):
    return key.public_key().public_bytes(serialization.Encoding.PEM,
                                         serialization.PublicFormat.SubjectPublicKeyInfo)

def public_key_material(pem_bytes):
    """Curve/RSA-agnostic key identity, robust to PEM formatting differences between encoders."""
    k = serialization.load_pem_public_key(pem_bytes)
    if isinstance(k, rsa.RSAPublicKey):
        n = k.public_numbers()
        return ("rsa", n.n, n.e)
    if isinstance(k, ec.EllipticCurvePublicKey):
        n = k.public_numbers()
        return ("ec", n.curve.name, n.x, n.y)
    if isinstance(k, ed25519.Ed25519PublicKey):
        return ("ed25519", k.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw))
    raise TypeError(type(k))

EC_CURVE_LEN = {"ec_p256": 32, "ec_p384": 48}

# scheme name -> (WFX key-type header value, cryptography hash class, RSA padding factory or None)
SCHEMES = {
    "rs256":   ("rsa", hashes.SHA256, lambda h: padding.PKCS1v15()),
    "rs384":   ("rsa", hashes.SHA384, lambda h: padding.PKCS1v15()),
    "rs512":   ("rsa", hashes.SHA512, lambda h: padding.PKCS1v15()),
    "ps256":   ("rsa", hashes.SHA256, lambda h: padding.PSS(mgf=padding.MGF1(h()), salt_length=h().digest_size)),
    "ps384":   ("rsa", hashes.SHA384, lambda h: padding.PSS(mgf=padding.MGF1(h()), salt_length=h().digest_size)),
    "ps512":   ("rsa", hashes.SHA512, lambda h: padding.PSS(mgf=padding.MGF1(h()), salt_length=h().digest_size)),
    "es256":   ("ec_p256", hashes.SHA256, None),
    "es384":   ("ec_p384", hashes.SHA384, None),
    "ed25519": ("ed25519", None, None),
}

def crypto_sign(priv, scheme_name, msg):
    """Signs with `cryptography`, returning bytes in WFX's own wire format (raw R||S for EC)."""
    kind, hash_cls, pad_fn = SCHEMES[scheme_name]
    if kind == "rsa":
        return priv.sign(msg, pad_fn(hash_cls), hash_cls())
    if kind == "ed25519":
        return priv.sign(msg)
    der = priv.sign(msg, ec.ECDSA(hash_cls()))
    r, s = asym_utils.decode_dss_signature(der)
    n = EC_CURVE_LEN[kind]
    return r.to_bytes(n, "big") + s.to_bytes(n, "big")

def crypto_verify(pub, scheme_name, msg, sig):
    """Verifies with `cryptography`. Returns True/False, never raises."""
    kind, hash_cls, pad_fn = SCHEMES[scheme_name]
    try:
        if kind == "rsa":
            pub.verify(sig, msg, pad_fn(hash_cls), hash_cls())
        elif kind == "ed25519":
            pub.verify(sig, msg)
        else:
            n = EC_CURVE_LEN[kind]
            if len(sig) != 2 * n:
                return False
            r = int.from_bytes(sig[:n], "big")
            s = int.from_bytes(sig[n:], "big")
            pub.verify(asym_utils.encode_dss_signature(r, s), msg, ec.ECDSA(hash_cls()))
        return True
    except InvalidSignature:
        return False

# Real, independently-maintained Ed25519 test vectors (Project Wycheproof,
# testvectors_v1/ed25519_test.json) - a curated subset spanning valid signatures, RFC 8032 known
# test vectors, and the edge cases "Taming the many EdDSAs" (eprint.iacr.org/2020/1244) showed most
# libraries get wrong: non-canonical/malleated (R,S) pairs, S just above the group order, truncated
# and garbage-appended signatures, and single-bit R corruption. Catches a systematically-wrong-but-
# self-consistent implementation that round-trip testing alone could never reveal
WYCHEPROOF_ED25519 = [
    # (tcId, flag, pk_hex, msg_hex, sig_hex, expect_valid, comment)
    (1, 'Valid', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '', 'd4fbdb52bfa726b44d1786a8c0d171c3e62ca83c9e5bbe63de0bb2483f8fd6cc1429ab72cafc41ab56af02ff8fcc43b99bfe4c7ae940f60f38ebaa9d311c4007', True, ''),
    (2, 'Valid', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '78', 'd80737358ede548acb173ef7e0399f83392fe8125b2ce877de7975d8b726ef5b1e76632280ee38afad12125ea44b961bf92f1178c9fa819d020869975bcbe109', True, ''),
    (10, 'InvalidSignature', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '3f', '00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000', False, 'special values for r and s'),
    (11, 'InvalidSignature', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '3f', '00000000000000000000000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000000000000000000000', False, 'special values for r and s'),
    (30, 'TruncatedSignature', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '54657374', '', False, 'empty signature'),
    (31, 'TruncatedSignature', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '54657374', '7c38e026f29e14aabd059a0f2db8b0cd783040609a8be684db12f82a27774ab0', False, 's missing'),
    (33, 'SignatureWithGarbage', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '54657374', '7c38e026f29e14aabd059a0f2db8b0cd783040609a8be684db12f82a27774ab07a9155711ecfaf7f99f277bad0c6ae7e39d4eef676573336a5c51eb6f946b30d2020', False, 'signature too long'),
    (34, 'SignatureWithGarbage', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '54657374', '7c38e026f29e14aabd059a0f2db8b0cd783040609a8be684db12f82a27774ab07a9155711ecfaf7f99f277bad0c6ae7e39d4eef676573336a5c51eb6f946b30d7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', False, 'include pk in signature'),
    (38, 'CompressedSignature', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '546573743137', '93de3ca252426c95f735cb9edd92e83321ac62372d5aa5b379786bae111ab6b17251330e8f9a7c30d6993137c596007d7b001409287535ac4804e662bc58a3', False, 'removing 0 byte from signature'),
    (39, 'CompressedSignature', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '54657374313236', 'dffed33a7f420b62bb1731cfd03be805affd18a281ec02b1067ba6e9d20826569e742347df59c88ae96db1f1969fb189b0ec34381d85633e1889da48d95e0e', False, 'removing 0 byte from signature'),
    (42, 'InvalidEncoding', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '313233343030', '647c1492402ab5ce03e2c3a7f0384d051b9cf3570f1207fc78c1bcc98c281c2b1d125e5538f38afbcc1c84e489521083041d24bc6240767029da063271a1ff0c', False, 'modified bit 0 in R'),
    (43, 'InvalidEncoding', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '313233343030', '677c1492402ab5ce03e2c3a7f0384d051b9cf3570f1207fc78c1bcc98c281c2bc108ca4b87a49c9ed2cf383aecad8f54a962b2899da891e12004d7993a627e01', False, 'modified bit 1 in R'),
    (63, 'SignatureMalleability', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '54657374', '7c38e026f29e14aabd059a0f2db8b0cd783040609a8be684db12f82a27774ab067654bce3832c2d76f8f6f5dafc08d9339d4eef676573336a5c51eb6f946b31d', False, 'checking malleability'),
    (64, 'SignatureMalleability', '7d4d0e7f6153a69b6242b522abbee685fda4420f8834b108c3bdae369ef549fa', '54657374', '7c38e026f29e14aabd059a0f2db8b0cd783040609a8be684db12f82a27774ab05439412b5395d42f462c67008eba6ca839d4eef676573336a5c51eb6f946b32d', False, 'checking malleability'),
    (80, 'Ktv', 'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a', '', 'e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b', True, 'draft-josefsson-eddsa-ed25519-02: Test 1'),
    (81, 'Ktv', '3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c', '72', '92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00', True, 'draft-josefsson-eddsa-ed25519-02: Test 2'),
    (85, 'InvalidKtv', '100fdf47fb94f1536a4f7c3fda27383fa03375a8f527c537e6f1703c47f94f86', '6a0bc2b0057cedfc0fa2e3f7f7d39279b30f454a69dfd1117c758d86b19d85e0', '0971f86d2c9c78582524a103cb9cf949522ae528f8054dc20107d999be673ff4e25ebf2f2928766b1248bec6e91697775f8446639ede46ad4df4053000000010', False, 'Signature with S just above the bound. [David Benjamin]'),
]

# PHASE: keygen + export round trip, including cross-loading a key WFX did not itself generate
def phase_asym_roundtrip(ctx):
    cfg = ctx.cfg
    p = ctx.phase("asym-roundtrip")

    for type_, bits in [("rsa", 2048), ("rsa", 3072), ("ec_p256", 0), ("ec_p384", 0), ("ed25519", 0)]:
        r = wfx_keygen(cfg, type_, bits)
        ok = bool(r) and r.get("status") == ST_OK
        p.check("keygen %s bits=%d" % (type_, bits), ok, "got %r" % r)
        if not ok:
            continue

        priv, pub = hexfield(r, "private_pem"), hexfield(r, "public_pem")
        p.check("keygen %s private PEM header" % type_,
              priv.startswith(b"-----BEGIN PRIVATE KEY-----"), "got %r" % priv[:40])
        p.check("keygen %s public PEM header" % type_,
              pub.startswith(b"-----BEGIN PUBLIC KEY-----"), "got %r" % pub[:40])

        try:
            serialization.load_pem_private_key(priv, password=None)
            serialization.load_pem_public_key(pub)
            p.check("keygen %s parses in `cryptography`" % type_, True)
        except Exception as exc:
            p.check("keygen %s parses in `cryptography`" % type_, False, repr(exc))
            continue

        bad = wfx_export(cfg, pub, key_is_private=False, export_private=True)
        p.check("%s export-private on public-only handle -> INVALID_ARG" % type_,
              bool(bad) and bad.get("status") == ST_INVALID_ARG, "got %r" % bad, security=True)

        reexport = wfx_export(cfg, priv, key_is_private=True, export_private=False)
        p.check("%s reload+re-export public key material matches" % type_,
              bool(reexport) and reexport.get("status") == ST_OK and
              public_key_material(hexfield(reexport)) == public_key_material(pub), "got %r" % reexport)

    # WFX must also be able to consume a key it did NOT generate itself
    externally_generated = [
        ("rsa", rsa.generate_private_key(public_exponent=65537, key_size=2048)),
        ("ec_p256", ec.generate_private_key(ec.SECP256R1())),
        ("ec_p384", ec.generate_private_key(ec.SECP384R1())),
        ("ed25519", ed25519.Ed25519PrivateKey.generate()),
    ]
    for type_, key in externally_generated:
        pp, qp = priv_pem(key), pub_pem(key)
        r = wfx_export(cfg, pp, key_is_private=True, export_private=False)
        p.check("%s externally-generated private key loads, re-exports matching public" % type_,
              bool(r) and r.get("status") == ST_OK and public_key_material(hexfield(r)) == public_key_material(qp),
              "got %r" % r)

# PHASE: sign/verify cross-checked against `cryptography` in both directions, for every scheme
def phase_asym_cross_oracle(ctx):
    cfg = ctx.cfg
    p = ctx.phase("asym-cross-oracle")
    msgs = [b"", b"hello wfx", os.urandom(1000), os.urandom(200000)]

    keys = {
        "rsa": rsa.generate_private_key(public_exponent=65537, key_size=2048),
        "ec_p256": ec.generate_private_key(ec.SECP256R1()),
        "ec_p384": ec.generate_private_key(ec.SECP384R1()),
        "ed25519": ed25519.Ed25519PrivateKey.generate(),
    }
    pems = {k: (priv_pem(v), pub_pem(v)) for k, v in keys.items()}

    for scheme, (kind, _, __) in SCHEMES.items():
        priv_key = keys[kind]
        pp, qp = pems[kind]

        for msg in msgs:
            sig_a = crypto_sign(priv_key, scheme, msg)
            r = wfx_verify(cfg, qp, scheme, msg, sig_a)
            p.check("%s cross-verify (cryptography->WFX) len=%d" % (scheme, len(msg)),
                  bool(r) and r.get("status") == ST_OK, "got %r" % r)

            r2 = wfx_sign(cfg, pp, scheme, msg)
            sig_ok = bool(r2) and r2.get("status") == ST_OK
            p.check("%s WFX sign len=%d" % (scheme, len(msg)), sig_ok, "got %r" % r2)
            if not sig_ok:
                continue

            sig_b = hexfield(r2)
            accepted = crypto_verify(keys[kind].public_key(), scheme, msg, sig_b)
            p.check("%s cross-verify (WFX->cryptography) len=%d" % (scheme, len(msg)), accepted,
                  "cryptography rejected WFX's own signature")

            if len(sig_b) > 0:
                tampered = bytearray(sig_b)
                tampered[-1] ^= 0x01
                rt = wfx_verify(cfg, qp, scheme, msg, bytes(tampered))
                p.check("%s tamper sig detected len=%d" % (scheme, len(msg)),
                      bool(rt) and rt.get("status") == ST_AUTH_FAILED, "got %r" % rt, security=True)

            if len(msg) > 0:
                tampered_msg = bytearray(msg)
                tampered_msg[0] ^= 0x01
                rt2 = wfx_verify(cfg, qp, scheme, bytes(tampered_msg), sig_a)
                p.check("%s tamper msg detected len=%d" % (scheme, len(msg)),
                      bool(rt2) and rt2.get("status") == ST_AUTH_FAILED, "got %r" % rt2, security=True)

# PHASE: algorithm/key confusion defenses (the API-level guard against RS256-vs-HS256-style attacks)
def phase_asym_key_confusion(ctx):
    cfg = ctx.cfg
    p = ctx.phase("asym-key-confusion")

    rsa_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    rsa_key2 = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ec256_key = ec.generate_private_key(ec.SECP256R1())
    ec384_key = ec.generate_private_key(ec.SECP384R1())
    ed_key = ed25519.Ed25519PrivateKey.generate()
    msg = b"algorithm confusion probe"

    mismatches = [
        ("es256", pub_pem(rsa_key), "RSA key, EC scheme"),
        ("rs256", pub_pem(ec256_key), "EC key, RSA scheme"),
        ("ed25519", pub_pem(ec256_key), "EC key, Ed25519 scheme"),
        ("rs256", pub_pem(ed_key), "Ed25519 key, RSA scheme"),
        ("es384", pub_pem(ec256_key), "P-256 key, ES384 scheme (curve mismatch)"),
        ("es256", pub_pem(ec384_key), "P-384 key, ES256 scheme (curve mismatch)"),
    ]
    dummy_sig = b"\x00" * 64
    for scheme, key_pem, label in mismatches:
        r = wfx_verify(cfg, key_pem, scheme, msg, dummy_sig)
        p.check("key/scheme mismatch rejected: %s" % label,
              bool(r) and r.get("status") == ST_INVALID_ARG, "got %r" % r, security=True)

    sig = crypto_sign(rsa_key, "rs256", msg)
    r = wfx_verify(cfg, pub_pem(rsa_key2), "rs256", msg, sig)
    p.check("wrong RSA key rejected (right scheme)", bool(r) and r.get("status") == ST_AUTH_FAILED,
          "got %r" % r, security=True)

    sig_rs = crypto_sign(rsa_key, "rs256", msg)
    r = wfx_verify(cfg, pub_pem(rsa_key), "ps256", msg, sig_rs)
    p.check("PKCS1 signature rejected under PSS scheme (same key)",
          bool(r) and r.get("status") == ST_AUTH_FAILED, "got %r" % r, security=True)

    sig_ps = crypto_sign(rsa_key, "ps256", msg)
    r = wfx_verify(cfg, pub_pem(rsa_key), "rs256", msg, sig_ps)
    p.check("PSS signature rejected under PKCS1 scheme (same key)",
          bool(r) and r.get("status") == ST_AUTH_FAILED, "got %r" % r, security=True)

    # ECDSA malleability: (r, n-s) is a second valid encoding of the same signature. Informational,
    # not a vulnerability here - JOSE/JWT has no canonical-signature requirement (unlike blockchain
    # use cases), so this pins down expected math rather than asserting a security property
    p256_order = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
    sig = crypto_sign(ec256_key, "es256", msg)
    s_int = int.from_bytes(sig[32:], "big")
    malleated = sig[:32] + ((p256_order - s_int) % p256_order).to_bytes(32, "big")
    r = wfx_verify(cfg, pub_pem(ec256_key), "es256", msg, malleated)
    p.check("ECDSA (r, n-s) malleated signature also verifies (expected, not a bug)",
          bool(r) and r.get("status") == ST_OK, "got %r" % r)

# PHASE: known-answer checks against real third-party Ed25519 test vectors (see WYCHEPROOF_ED25519)
def phase_asym_wycheproof(ctx):
    cfg = ctx.cfg
    p = ctx.phase("asym-wycheproof")
    security_flags = {"InvalidSignature", "SignatureMalleability", "InvalidKtv", "InvalidEncoding",
                       "SignatureWithGarbage", "CompressedSignature", "TruncatedSignature"}

    for tc_id, flag, pk_hex, msg_hex, sig_hex, expect_valid, comment in WYCHEPROOF_ED25519:
        pub = ed25519.Ed25519PublicKey.from_public_bytes(bytes.fromhex(pk_hex))
        pub_pem_bytes = pub.public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
        msg = bytes.fromhex(msg_hex)

        r = wfx_verify(cfg, pub_pem_bytes, "ed25519", msg, sig_hex)
        accepted = bool(r) and r.get("status") == ST_OK
        p.check("wycheproof ed25519 #%d [%s] %s" % (tc_id, flag, comment),
              accepted == expect_valid, "got %r want valid=%s" % (r, expect_valid),
              security=(flag in security_flags))

# PHASE: adversarial inputs at the raw-component construction boundary (JWKS-sourced key material)
def phase_asym_hostile(ctx):
    cfg = ctx.cfg
    p = ctx.phase("asym-hostile")

    # Invalid curve point (y doesn't satisfy the curve equation for this x) - exercises the
    # EVP_PKEY_public_check gate added around raw-component key construction, since
    # EVP_PKEY_fromdata() never validates a point is actually on the curve on its own
    good_x = int.from_bytes(os.urandom(32), "big")
    bad_point = wfx_from_ec_public(cfg, "p256", good_x.to_bytes(32, "big"), os.urandom(32))
    p.check("EC point not on curve rejected", bool(bad_point) and bad_point.get("status") != ST_OK,
          "got %r" % bad_point, security=True)

    zero_point = wfx_from_ec_public(cfg, "p256", b"\x00" * 32, b"\x00" * 32)
    p.check("EC all-zero point rejected", bool(zero_point) and zero_point.get("status") != ST_OK,
          "got %r" % zero_point, security=True)

    p256_key = ec.generate_private_key(ec.SECP256R1())
    nums = p256_key.public_key().public_numbers()
    wrong_curve = wfx_from_ec_public(cfg, "p384", nums.x.to_bytes(32, "big"), nums.y.to_bytes(32, "big"))
    p.check("P-256 point rejected under P-384 claim", bool(wrong_curve) and wrong_curve.get("status") != ST_OK,
          "got %r" % wrong_curve, security=True)

    real_n = rsa.generate_private_key(public_exponent=65537, key_size=2048).public_key().public_numbers().n
    real_n_bytes = real_n.to_bytes(256, "big")
    for label, n_bytes, e_bytes in [
        ("e=0", real_n_bytes, b"\x00"),
        ("e=1", real_n_bytes, b"\x01"),
        ("n=0", b"\x00", b"\x01\x00\x01"),
        ("n=1", b"\x01", b"\x01\x00\x01"),
    ]:
        r = wfx_from_rsa_public(cfg, n_bytes, e_bytes)
        p.check("degenerate RSA key rejected (%s)" % label, bool(r) and r.get("status") != ST_OK,
              "got %r" % r, security=True)

    # Not a valid RSA key (random bytes, no real semiprime structure), but must fail fast rather
    # than hang or exhaust memory trying to validate an attacker-sized modulus
    huge_n = os.urandom(1_000_000)
    start = time.time()
    r = wfx_from_rsa_public(cfg, huge_n, b"\x01\x00\x01", rtimeout=20.0)
    elapsed = time.time() - start
    p.check("1MB bogus RSA modulus handled without hanging (%.1fs)" % elapsed,
          r is not None and elapsed < 15.0, "got %r after %.1fs" % (r, elapsed), security=True)

def make_jwks(entries):
    return json.dumps({"keys": entries}).encode()

def b64url(b):
    import base64
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()

def rsa_jwk(kid, key):
    n = key.public_key().public_numbers()
    return {"kty": "RSA", "kid": kid, "n": b64url(n.n.to_bytes((n.n.bit_length() + 7) // 8, "big")),
            "e": b64url(n.e.to_bytes((n.e.bit_length() + 7) // 8, "big"))}

def ec_jwk(kid, key, crv):
    n = key.public_key().public_numbers()
    width = EC_CURVE_LEN["ec_p256" if crv == "P-256" else "ec_p384"]
    return {"kty": "EC", "kid": kid, "crv": crv,
            "x": b64url(n.x.to_bytes(width, "big")), "y": b64url(n.y.to_bytes(width, "big"))}

# PHASE: JWKS parsing - normal lookup, malformed-shape survival, kid-confusion, oversized body
def phase_jwk(ctx):
    cfg = ctx.cfg
    p = ctx.phase("jwk")

    rsa_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ec_key = ec.generate_private_key(ec.SECP256R1())
    jwks = make_jwks([rsa_jwk("rsa-1", rsa_key), ec_jwk("ec-1", ec_key, "P-256")])

    r = wfx_jwk_load(cfg, jwks, "rsa-1")
    p.check("jwk load RSA by kid", bool(r) and r.get("status") == ST_OK, "got %r" % r)
    if r and r.get("status") == ST_OK:
        loaded = serialization.load_pem_public_key(hexfield(r))
        p.check("jwk-loaded RSA key matches source",
              loaded.public_numbers() == rsa_key.public_key().public_numbers(), "mismatch")

    r = wfx_jwk_load(cfg, jwks, "ec-1")
    p.check("jwk load EC by kid", bool(r) and r.get("status") == ST_OK, "got %r" % r)

    r = wfx_jwk_load(cfg, jwks, "does-not-exist")
    p.check("jwk load unknown kid -> INVALID_ARG", bool(r) and r.get("status") == ST_INVALID_ARG, "got %r" % r)

    malformed = [
        (b'{"keys": "not-an-array"}', "keys not array"),
        (b'{"nope": []}', "missing keys field"),
        (b'{"keys": [{"kty": "RSA", "kid": "x"}]}', "RSA missing n/e"),
        (b'{"keys": [{"kty": "EC", "kid": "x", "crv": "P-256"}]}', "EC missing x/y"),
        (b'{"keys": [{"kty": "UNKNOWN", "kid": "x"}]}', "unknown kty"),
        (b'{"keys": [{"kty": "RSA", "kid": "x", "n": "!!!not-base64!!!", "e": "AQAB"}]}', "invalid base64url n"),
        (b'{"keys": []}', "empty keys array"),
        (b'not even json', "invalid JSON"),
        (b'', "empty body"),
    ]
    for body, label in malformed:
        r = call(cfg, "POST", "/jwk/load", {"X-Kid": "x"}, body)
        p.check("jwk malformed survives: %s" % label, r is not None and "status" in r, "got %r" % r)

    # JWKS confusion: two entries share a kid, one legitimate and one attacker-supplied. Lookup
    # must be deterministic (first match wins) rather than picking whichever entry an attacker
    # managed to have sorted or inserted differently
    attacker_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    dup_jwks = make_jwks([rsa_jwk("shared-kid", rsa_key), rsa_jwk("shared-kid", attacker_key)])
    r = wfx_jwk_load(cfg, dup_jwks, "shared-kid")
    p.check("duplicate kid resolves to first entry",
          bool(r) and r.get("status") == ST_OK and
          serialization.load_pem_public_key(hexfield(r)).public_numbers() == rsa_key.public_key().public_numbers(),
          "got %r" % r, security=True)

    decoys = [rsa_jwk("decoy-%d" % i, rsa.generate_private_key(public_exponent=65537, key_size=1024))
              for i in range(50)]
    big_jwks = make_jwks(decoys + [rsa_jwk("real", rsa_key)])
    start = time.time()
    r = wfx_jwk_load(cfg, big_jwks, "real", rtimeout=20.0)
    elapsed = time.time() - start
    p.check("JWKS with 50 decoys resolves correctly and quickly (%.1fs)" % elapsed,
          bool(r) and r.get("status") == ST_OK and elapsed < 10.0, "got %r after %.1fs" % (r, elapsed))

# PHASE: Base64 / Hex / Url encoding, functional round trips plus adversarial decode inputs
def phase_encoding(ctx):
    cfg = ctx.cfg
    p = ctx.phase("encoding")
    import base64

    def b64_encode(data, url_safe=False, padded=True):
        return call(cfg, "POST", "/encoding/base64/encode",
                    {"X-Urlsafe": "1" if url_safe else "0", "X-Padded": "1" if padded else "0"}, data)

    def as_bytes(text):
        return text.encode() if isinstance(text, str) else text

    def b64_decode(text):
        return call(cfg, "POST", "/encoding/base64/decode", {}, as_bytes(text))

    def hex_encode(data, upper=False):
        return call(cfg, "POST", "/encoding/hex/encode", {"X-Upper": "1" if upper else "0"}, data)

    def hex_decode(text):
        return call(cfg, "POST", "/encoding/hex/decode", {}, as_bytes(text))

    def url_encode(data):
        return call(cfg, "POST", "/encoding/url/encode", {}, data)

    def url_decode(text):
        return call(cfg, "POST", "/encoding/url/decode", {}, as_bytes(text))

    for n in (0, 1, 2, 3, 4, 100, 10000):
        data = os.urandom(n)
        for url_safe in (False, True):
            for padded in (False, True):
                expect = (base64.urlsafe_b64encode(data) if url_safe else base64.b64encode(data)).decode()
                if not padded:
                    expect = expect.rstrip("=")
                r = b64_encode(data, url_safe, padded)
                p.check("base64 encode len=%d url=%s pad=%s" % (n, url_safe, padded),
                      bool(r) and r.get("b64") == expect, "got %r want %r" % (r, expect))

                rd = b64_decode(r["b64"]) if r else None
                p.check("base64 decode round-trip len=%d url=%s pad=%s" % (n, url_safe, padded),
                      bool(rd) and rd.get("ok") and bytes.fromhex(rd.get("hex", "")) == data, "got %r" % rd)

    # Decode leniency: the shared char table accepts both '+/' and '-_' unconditionally - not
    # spec-strict, but documented, intentional behavior worth pinning down here so it isn't
    # changed by accident later without someone noticing
    rd = b64_decode("+-_/")
    p.check("base64 decode accepts mixed standard/url-safe alphabet", bool(rd) and rd.get("ok") is True,
          "got %r" % rd)

    # "====" is not on this list - an all-padding string legitimately decodes to empty bytes
    # (confirmed against Python's own base64.b64decode, not a WFX-specific leniency). '=' in a
    # non-trailing position is the genuinely invalid shape: only a *trailing* run of '=' is
    # stripped as padding, so an embedded one is just an unrecognized character
    for bad in ("!!!!", "abc def", "a\x00bc=", "AB=A"):
        rd = b64_decode(bad)
        p.check("base64 decode rejects invalid input: %r" % bad, bool(rd) and rd.get("ok") is False,
              "got %r" % rd)

    # Sized to stay under send_buffer_max (~1.05MiB, see app/wfx.toml) after base64's ~4/3 expansion,
    # this is a timing/robustness sanity check, not a send-buffer-limits test
    big = os.urandom(700 * 1024)
    start = time.time()
    r = b64_encode(big)
    elapsed = time.time() - start
    expect_b64 = base64.b64encode(big).decode()
    p.check("base64 encode large payload completes quickly and correctly (%.2fs)" % elapsed,
          bool(r) and r.get("b64") == expect_b64 and elapsed < 10.0,
          "elapsed=%.2fs match=%s" % (elapsed, bool(r) and r.get("b64") == expect_b64))

    for n in (0, 1, 2, 100, 10000):
        data = os.urandom(n)
        for upper in (False, True):
            expect = data.hex().upper() if upper else data.hex()
            r = hex_encode(data, upper)
            p.check("hex encode len=%d upper=%s" % (n, upper), bool(r) and r.get("hex") == expect,
                  "got %r want %r" % (r, expect))

        r = hex_decode(data.hex())
        p.check("hex decode round-trip (lower) len=%d" % n,
              bool(r) and r.get("ok") and r.get("hex") == data.hex(), "got %r" % r)
        r = hex_decode(data.hex().upper())
        p.check("hex decode round-trip (upper) len=%d" % n,
              bool(r) and r.get("ok") and r.get("hex") == data.hex(), "got %r" % r)

    for bad in ("0", "zz", "gg", "12345", "  "):
        r = hex_decode(bad)
        p.check("hex decode rejects invalid input: %r" % bad, bool(r) and r.get("ok") is False, "got %r" % r)

    for raw in (b"", b"hello world", b"a-b_c.d~e", bytes(range(256))):
        r = url_encode(raw)
        p.check("url encode len=%d" % len(raw), bool(r) and "url" in r, "got %r" % r)
        if not r:
            continue
        rd = url_decode(r["url"])
        p.check("url decode round-trip len=%d" % len(raw),
              bool(rd) and rd.get("ok") and bytes.fromhex(rd.get("hex", "")) == raw, "got %r" % rd)

    r = url_decode("a+b")
    p.check("url decode '+' tolerated as space",
          bool(r) and r.get("ok") and bytes.fromhex(r.get("hex", "")) == b"a b", "got %r" % r)

    for bad in ("%", "%4", "%zz", "100%"):
        r = url_decode(bad)
        p.check("url decode rejects truncated/invalid escape: %r" % bad,
              bool(r) and r.get("ok") is False, "got %r" % r)

class CryptoAudit(common.Suite):
    name = "crypto_audit"
    description = "WFX crypto ABI audit"
    phases = {
        "hash":              phase_hash,
        "stream-response":   phase_stream_response,
        "hmac":              phase_hmac,
        "aead":              phase_aead,
        "aead-cap":          phase_aead_cap,
        "kdf":               phase_kdf,
        "misc":              phase_misc,
        "asym-roundtrip":    phase_asym_roundtrip,
        "asym-cross-oracle": phase_asym_cross_oracle,
        "asym-key-confusion": phase_asym_key_confusion,
        "asym-wycheproof":   phase_asym_wycheproof,
        "asym-hostile":      phase_asym_hostile,
        "jwk":               phase_jwk,
        "encoding":          phase_encoding,
    }

if __name__ == "__main__":
    common.run(CryptoAudit)
