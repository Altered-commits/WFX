# WFX Crypto Audit

Correctness testing for the `wfx/utils/crypto.hpp` ABI: hashing, HMAC, AEAD,
key derivation, CSPRNG, constant-time comparison, asymmetric sign/verify, JWKS
loading, and Base64/Hex/URL encoding. Boots the server, drives every crypto
route, checks results against Python oracles where one exists (`hashlib`,
`hmac`, PBKDF2, a hand-rolled RFC 5869 HKDF, the `cryptography` package for
asymmetric schemes), stops the server.

---

## Requirements

- `wfx` on `PATH` (or pass `--wfx /path/to/wfx`)
- Python 3.8+, plus the third-party [`cryptography`](https://pypi.org/project/cryptography/)
  package (`pip install cryptography`) — needed by the `asym-*` phases, everything
  else uses only the standard library
- Linux with `/proc` (for worker PID discovery)

---

## Quick start

```bash
cd tests/crypto_audit

# All phases
python3 crypto_audit.py

# Single phase
python3 crypto_audit.py --phase hash
python3 crypto_audit.py --phase aead
python3 crypto_audit.py --phase asym-roundtrip
python3 crypto_audit.py --phase jwk

# Different binary or port
python3 crypto_audit.py --wfx /path/to/wfx --port 9090

# GitHub Actions
python3 crypto_audit.py --ci
```

---

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | All phases passed |
| `1` | Correctness failure |
| `2` | **Security**: an AEAD forgery was accepted (tampered ciphertext, wrong AAD, cross-algorithm key/nonce), a tampered asymmetric signature or message verified, a key/scheme confusion was accepted, or a JWKS duplicate-kid lookup resolved to the wrong entry |
| `3` | Server never came up |

---

## Phases

### hash

One-shot (`WFX::Sha256/384/512`) and streaming (`WFX::HashStream<Algo>`)
hashing, checked against `hashlib` for empty, short, random, and 100 KB
inputs. Streaming results must match the one-shot digest exactly.

### stream-response

Exercises `WFX::HashStream<Algo>` driven from *inside* a `res.Stream()`
callback: the server streams deterministic content out through the response
body while hashing it chunk-by-chunk as each callback invocation produces
bytes, then appends the finished digest in-band (`\n#DIGEST:<hex>\n`, since
there's no HTTP trailer mechanism exposed). The audit reconstructs the
expected content client-side, de-chunks the transfer-encoded response itself
(stdlib-only, no `http.client`), and checks both the streamed bytes and the
streaming-computed digest.

### hmac

One-shot (`WFX::HmacSha256/384/512`) and streaming (`WFX::HmacStream<Algo>`)
HMAC, checked against Python's `hmac` module, including an RFC 4231 test
vector.

### aead

`WFX::AeadEncrypt`/`AeadDecrypt` for both AES-256-GCM and ChaCha20-Poly1305:
round-trip correctness across a range of plaintext sizes, tamper detection
(flipped ciphertext byte → `AUTH_FAILED`), wrong-AAD rejection, cross-algo
rejection (ciphertext from one algo must not decrypt under the other), and
short key/nonce → `INVALID_ARG`. No stdlib AEAD oracle exists, so these are
structural checks rather than byte-exact ones.

### aead-cap

Sends a 64 MiB + 1 byte plaintext and checks `WFX::AeadEncrypt` rejects it
with `INVALID_ARG` (`CryptoAeadMaxSize`), without ever performing the
encryption. `wfx.toml`'s `max_body_size` is raised specifically to let a body
this large reach the route at all.

### kdf

`WFX::Pbkdf2` checked against `hashlib.pbkdf2_hmac`; `WFX::Hkdf` checked
against a hand-rolled RFC 5869 HKDF-SHA256 (Python stdlib has no HKDF).
`WFX::Argon2id` has no stdlib oracle, so it's checked structurally:
deterministic given identical inputs, correct output length, and a changed
salt producing a different output.

### misc

`WFX::RandomBytes`: correct length, differs across calls, not all-zero, and
`len=0` specifically checked as `INVALID_ARG` (an explicit zero-length check,
distinct from the null-pointer checks the hash/HMAC/AEAD empty-input cases go
through). `WFX::ConstantTimeEquals`: equal/unequal/different-length comparisons.

### asym-roundtrip

Key generation for every supported type (RSA 2048/3072, EC P-256/P-384,
Ed25519): correct PEM headers, parses in `cryptography`, private-key export
refused on a public-only handle, and a key `cryptography` generated itself
(not WFX) loads and re-exports its public half correctly.

### asym-cross-oracle

Sign/verify cross-checked against `cryptography` in both directions for every
scheme, across empty/short/large messages: WFX must verify a signature
`cryptography` made, `cryptography` must verify a signature WFX made, and a
tampered signature or tampered message must fail verification.

### asym-key-confusion

Algorithm/key confusion defenses: a key of the wrong type for a scheme, the
wrong key under the right scheme, a PKCS1 signature rejected under PSS (and
vice versa) with the same key, and ECDSA `(r, n-s)` malleability confirmed as
expected math, not a vulnerability (JOSE/JWT has no canonical-signature
requirement).

### asym-wycheproof

Known-answer checks against real third-party Ed25519 test vectors, including
malleability, invalid-encoding, and truncated-signature edge cases.

### asym-hostile

Adversarial raw-component key construction: an EC point not on the curve, an
all-zero point, a point claimed under the wrong curve, degenerate RSA
moduli/exponents (`e=0`, `n=0`, ...), and a 1 MB bogus RSA modulus that must
fail fast rather than hang or exhaust memory.

### jwk

JWKS parsing: lookup by `kid` for both RSA and EC keys, unknown-`kid`
rejection, a malformed-shape corpus (non-array `keys`, missing fields, unknown
`kty`, invalid base64url, empty/invalid body) that must survive without
crashing, a duplicate-`kid` JWKS resolving deterministically to the first
entry (not whichever one an attacker managed to insert), and a 50-decoy JWKS
that must still resolve the real key correctly and quickly.

### encoding

Base64 (standard/url-safe, padded/unpadded), Hex, and URL encode/decode round
trips checked against Python's own codecs, decode leniency (the shared
lookup table accepts a mixed standard/url-safe alphabet, by design), rejection
of genuinely invalid input, and a large-payload timing sanity check.

---

## Test app (`app/`)

A minimal WFX project exposing one route per crypto primitive (see
`app/src/main.cpp`). Binary inputs (keys, nonces, AAD, salts, IKM, info,
passwords) travel as hex-encoded headers; the primary data blob (hash/HMAC
input, AEAD plaintext/ciphertext) travels as the raw request body.
