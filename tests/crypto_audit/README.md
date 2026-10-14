# WFX Crypto Audit

Correctness testing for the `wfx/utils/crypto.hpp`, `jwk.hpp`, `jwt.hpp` and
`encoding.hpp` ABIs: hashing, HMAC, AEAD, key derivation, CSPRNG, constant-time
comparison, asymmetric sign/verify, JWKS loading, compact JWT parsing/verification,
and Base64/Hex/URL encoding. Boots the server, drives every crypto route, checks
results against Python oracles
where one exists (`hashlib`, `hmac`, PBKDF2, a hand-rolled RFC 5869 HKDF, the
`cryptography` package for asymmetric schemes), stops the server.

---

## Requirements

- WFX installed with `./scripts/install.sh --local-debug` (puts `wfx` on `PATH`, its headers
  under `~/.wfx/src`, which `app/CMakeLists.txt` builds against, and ASan/UBSan on), or run
  through `tests/run_audits.sh` after that install
- Python 3.8+, plus the third-party [`cryptography`](https://pypi.org/project/cryptography/)
  package (`pip install cryptography`). It's imported at startup, so the suite needs it to run
  at all; the `asym-*`, `jwk` and `jwt` phases use it as their oracle
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
python3 crypto_audit.py --phase jwt

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
| `1` | A check failed, a phase timed out or raised, a sanitizer report was written, a worker ignored SIGTERM at shutdown, or the server was dead at the end |
| `2` | **Security**: an AEAD forgery was accepted (tampered ciphertext, wrong AAD, cross-algorithm key/nonce), a tampered asymmetric signature or message verified, a key/scheme confusion was accepted, a private key exported from a public-only handle, a hostile key component (off-curve or zero EC point, wrong-curve claim, degenerate RSA, an oversized modulus that hangs) accepted, a JWKS duplicate-kid lookup resolved to the wrong entry, or a tampered/wrong-key/alg-confused JWT verified |
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
encryption. `app/config/wfx.local.toml`'s `max_body_size` is raised specifically to let a body
this large reach the route at all.

### kdf

`WFX::Pbkdf2` checked against `hashlib.pbkdf2_hmac`; `WFX::Hkdf` checked
against a hand-rolled RFC 5869 HKDF-SHA256 (Python stdlib has no HKDF).
`WFX::Argon2id` has no stdlib oracle, so it's checked structurally:
deterministic given identical inputs, correct output length, and a changed
salt producing a different output.

### misc

`WFX::RandomBytes`: correct length, differs across calls, not all-zero, `len=0`
returns OK with an empty result, and two workers respawned after a SIGKILL never
repeat each other's bytes. `WFX::ConstantTimeEquals`: equal/unequal/different-length
comparisons. Malformed hex headers on the HMAC and AEAD routes must not crash the
worker.

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

### jwt

`wfx/utils/jwt.hpp`: a valid token round trip for every asymmetric scheme
(parse, `alg` reported correctly, audience and time claims both valid),
tamper detection on both the signature and the payload (old signature paired
with a changed payload), alg confusion (`none` with the signature segment
stripped entirely, `HS256`, and garbage `alg` values, all of which must be
refused as unsupported before the key is ever touched), expired/not-yet-valid/
missing-`exp` tokens, audience mismatches in both the bare-string and array
forms of `aud`, a validly-signed token checked against the wrong key, and a
malformed-token corpus (wrong segment count, invalid base64url, valid base64
but non-JSON header/payload) that must be rejected without crashing.

### encoding

Base64 (standard/url-safe, padded/unpadded), Hex, and URL encode/decode round
trips checked against Python's own codecs, decode leniency (the shared
lookup table accepts a mixed standard/url-safe alphabet, by design), rejection
of genuinely invalid input, and a large-payload timing sanity check.

### out-buffer

The `std::span` forms of AEAD, Base64 and Hex. The allocating forms are built on
them, so the phases above already prove what they compute; this one covers what
only the caller's buffer shows. An exact-size buffer reports the right written
count, a buffer one byte short is refused before anything is written, and a
tampered AEAD message leaves no plaintext in the buffer, not even a prefix
(security). A decode into a too-small buffer is refused; an overrun would surface
as a sanitizer report.

---

## Test app (`app/`)

A minimal WFX project exposing one route per crypto primitive (see
`app/src/main.cpp`). Binary inputs (keys, nonces, AAD, salts, IKM, info,
passwords) travel as hex-encoded headers; the primary data blob (hash/HMAC
input, AEAD plaintext/ciphertext) travels as the raw request body.
