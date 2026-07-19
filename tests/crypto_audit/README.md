# WFX Crypto Audit

Correctness testing for the `wfx/utils/crypto.hpp` ABI: hashing, HMAC, AEAD,
key derivation, CSPRNG, and constant-time comparison. Boots the server, drives
every crypto route, checks results against Python stdlib oracles where one
exists (hashlib, hmac, PBKDF2, a hand-rolled RFC 5869 HKDF), stops the server.

---

## Requirements

- `wfx` on `PATH` (or pass `--wfx /path/to/wfx`)
- Python 3.8+, standard library only
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
| `2` | **Security**: an AEAD forgery was accepted, i.e. tampered ciphertext, a wrong AAD, or a cross-algorithm key/nonce decrypted instead of failing authentication |
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

---

## Test app (`app/`)

A minimal WFX project exposing one route per crypto primitive (see
`app/src/main.cpp`). Binary inputs (keys, nonces, AAD, salts, IKM, info,
passwords) travel as hex-encoded headers; the primary data blob (hash/HMAC
input, AEAD plaintext/ciphertext) travels as the raw request body.
