# Crypto & Hash

WFX ships two separate toolkits under this page:

- [**Crypto**](#crypto): cryptographic hashing, HMAC, AEAD encryption, key derivation, CSPRNG, and constant-time comparison, backed by the engine's crypto ABI (OpenSSL today). Use this for secrets, tokens, passwords, and authenticated encryption.
- [**Hash**](#hash): fast, non-cryptographic hashing for hash tables, cache keys, checksums, and rolling windows. Runs entirely in user code, no engine call involved.

Do not use `Hash` functions for anything security-sensitive: they are not collision or preimage resistant against an adversary. Do not use `Crypto` hashing where raw speed matters more than cryptographic guarantees; use `Hash` instead.

---

## Crypto

!!! important
    All crypto functionality lives directly in the `WFX::` namespace.

    ```cpp
    #include <wfx/utils/crypto.hpp>
    ```

### Overview

Every operation crosses into the engine via `Core::CryptoApiExt1()` and returns a `{CryptoStatus, result}` pair (or, for streaming APIs, a `CryptoStatus` alone). Always check the status before touching the result.

```cpp
enum class CryptoStatus : std::uint8_t {
    OK = 0,
    INVALID_ARG,      // Null pointer, or key/nonce/output length doesn't match the algo
    BUFFER_TOO_SMALL, // Caller-provided output buffer is too small
    AUTH_FAILED,      // AEAD tag verification failed on decrypt; out is left untouched
    UNSUPPORTED,      // Backend doesn't provide this algorithm (e.g. Argon2 provider missing)
    INTERNAL_ERROR    // Underlying crypto library failure
};

inline constexpr auto CryptoOk = CryptoStatus::OK; // provided for convenience
```

!!! note
    Zero-length input (e.g. `Sha256("")`, `RandomBytes(0)`) is a legitimate empty input, not an error, and returns `CryptoOk`. `INVALID_ARG` only fires for a genuinely malformed call, such as a null pointer paired with a nonzero length, or a key/nonce of the wrong size for the algorithm.

---

### Hashing

#### One-shot

```cpp
auto [status, digest] = WFX::Sha256(data);   // also: Sha384, Sha512
```

- `data` (`std::string_view`): the bytes to hash
- Returns `std::pair<CryptoStatus, Digest<N>>`, where `N` is 32/48/64 for SHA256/384/512

`Digest<N>` is a small fixed-size wrapper:

```cpp
template <std::uint32_t N> struct Digest {
    std::array<std::uint8_t, N> bytes{};
    std::uint32_t len = 0;

    std::string_view View() const noexcept; // {bytes.data(), len}
};
```

**Example**:
```cpp
WFX_GET("/checksum", [](WFX::Request req, WFX::Response res) {
    auto [status, digest] = WFX::Sha256(req.Body());

    if(status != WFX::CryptoOk) {
        res.Status(WFX::HttpStatus::INTERNAL_SERVER_ERROR).SendText("hash failed");
        return;
    }

    res.SendText(digest.View()); // raw digest bytes, not hex
});
```

The generic form can also be called directly with an explicit algorithm:

```cpp
auto [status, digest] = WFX::Hash<WFX::CryptoHashAlgo::SHA512>(data);
```

#### Streaming

Use `HashStream<Algo>` when data arrives incrementally (e.g. across multiple `Response::Stream()` callback invocations) instead of as one in-memory buffer.

```cpp
template <CryptoHashAlgo Algo> class HashStream {
public:
    HashStream();                    // creates a fresh hash context
    bool Valid() const noexcept;     // false if context creation failed

    CryptoStatus Update(std::string_view data);
    std::pair<CryptoStatus, Digest<DigestLenFor<Algo>()>> Final();
};
```

- Move-only, so it's safe to capture inside a `Response::Stream()` lambda
- `Update()` may be called any number of times
- `Final()` finalizes the digest; the stream should not be updated afterward

**Example**:
```cpp
WFX::HashStream<WFX::CryptoHashAlgo::SHA256> hasher;

hasher.Update("hello ");
hasher.Update("world");

auto [status, digest] = hasher.Final();
```

!!! note
    Always check `.Valid()` after construction, and check the `CryptoStatus` from every `Update`/`Final` call. A failed context creation degrades every subsequent call to `INVALID_ARG` instead of crashing.

---

### HMAC

#### One-shot

```cpp
auto [status, mac] = WFX::HmacSha256(key, data);   // also: HmacSha384, HmacSha512
```

Same `Digest<N>` result type and status semantics as hashing. Generic form: `WFX::Hmac<Algo>(key, data)`.

#### Streaming

`HmacStream<Algo>` mirrors `HashStream<Algo>`, except the key is bound at construction:

```cpp
template <CryptoHashAlgo Algo> class HmacStream {
public:
    explicit HmacStream(std::string_view key);
    bool Valid() const noexcept;

    CryptoStatus Update(std::string_view data);
    std::pair<CryptoStatus, Digest<DigestLenFor<Algo>()>> Final();
};
```

**Example (verifying a token)**:
```cpp
auto [status, mac] = WFX::HmacSha256(secretKey, payload);

if(status != WFX::CryptoOk || !WFX::ConstantTimeEquals(mac.View(), providedMac)) {
    res.Status(WFX::HttpStatus::UNAUTHORIZED).SendText("invalid signature");
    return;
}
```

---

### AEAD (authenticated encryption)

```cpp
inline constexpr auto CryptoAesGcm            = CryptoAeadAlgo::AES_256_GCM;
inline constexpr auto CryptoChaCha20Poly1305  = CryptoAeadAlgo::CHACHA20_POLY1305;

std::pair<CryptoStatus, Vector<std::uint8_t>> AeadEncrypt(
    CryptoAeadAlgo algo, std::string_view key, std::string_view nonce,
    std::string_view aad, std::string_view plaintext);

std::pair<CryptoStatus, Vector<std::uint8_t>> AeadDecrypt(
    CryptoAeadAlgo algo, std::string_view key, std::string_view nonce,
    std::string_view aad, std::string_view ciphertext);
```

`key`/`nonce` must match the algorithm's exact size, or `INVALID_ARG` is returned:

| Algorithm | Key | Nonce | Tag |
|---|---|---|---|
| `AES_256_GCM` | 32 bytes | 12 bytes | 16 bytes |
| `CHACHA20_POLY1305` | 32 bytes | 12 bytes | 16 bytes |

- `aad` (additional authenticated data) is authenticated but not encrypted; pass an empty `std::string_view` if unused
- `AeadEncrypt` output is `ciphertext || tag` (tag appended, not separate)
- `AeadDecrypt` expects that same `ciphertext || tag` layout as input
- On `AUTH_FAILED` (tag mismatch), `AeadDecrypt` returns an empty vector; unauthenticated plaintext is never exposed, even partially

!!! warning
    AEAD is one-shot only. Both input and output live fully in memory. Inputs larger than `WFX::CryptoAeadMaxSize` (64 MiB) are rejected with `INVALID_ARG` up front, to fail loudly on an accidentally huge body instead of silently costing 2x its RAM. This is a hard cap, not a soft default. Genuinely large payloads need chunked, independently-authenticated framing (e.g. libsodium's `crypto_secretstream`), which WFX does not currently provide.

!!! danger
    Never reuse a `(key, nonce)` pair for two different messages. For AES-256-GCM and ChaCha20-Poly1305, a random 12-byte nonce is only safe up to roughly 2^32 messages under one key before collision risk becomes non-negligible. High-volume callers should use a counter-based nonce instead of pure randomness.

**Example**:
```cpp
auto [ks, key]   = WFX::RandomBytes(32); // AES-256-GCM key
auto [ns, nonce] = WFX::RandomBytes(12); // AES-256-GCM nonce

auto [es, ciphertext] = WFX::AeadEncrypt(
    WFX::CryptoAesGcm,
    std::string_view(reinterpret_cast<char*>(key.data()), key.size()),
    std::string_view(reinterpret_cast<char*>(nonce.data()), nonce.size()),
    /* aad */ "", plaintext);

if(es != WFX::CryptoOk) {
    // handle failure
}

auto [ds, decrypted] = WFX::AeadDecrypt(
    WFX::CryptoAesGcm,
    std::string_view(reinterpret_cast<char*>(key.data()), key.size()),
    std::string_view(reinterpret_cast<char*>(nonce.data()), nonce.size()),
    /* aad */ "",
    std::string_view(reinterpret_cast<char*>(ciphertext.data()), ciphertext.size()));

if(ds != WFX::CryptoOk) {
    // tag mismatch or malformed input, treat as tampered
}
```

---

### Key derivation

```cpp
std::pair<CryptoStatus, Vector<std::uint8_t>> Pbkdf2(
    std::string_view password, std::string_view salt,
    std::uint32_t iterations, std::uint32_t outLen);

std::pair<CryptoStatus, Vector<std::uint8_t>> Hkdf(
    std::string_view ikm, std::string_view salt,
    std::string_view info, std::uint32_t outLen);

std::pair<CryptoStatus, Vector<std::uint8_t>> Argon2id(
    std::string_view password, std::string_view salt,
    std::uint32_t iterations, std::uint32_t memoryKb,
    std::uint32_t parallelism, std::uint32_t outLen);
```

- **`Pbkdf2`**: PBKDF2-HMAC-SHA256 (RFC 8018). Writes exactly `outLen` bytes.
- **`Hkdf`**: HKDF-SHA256, extract-then-expand (RFC 5869). Writes exactly `outLen` bytes. `info` may be empty.
- **`Argon2id`**: password hashing (RFC 9106). `memoryKb` is the m_cost parameter in KiB, `iterations` is t_cost, `parallelism` is the lane count. Returns `UNSUPPORTED` if the backend's Argon2 provider isn't available.

All three return an empty vector on any non-`OK` status.

**Example (deriving a session key from a shared secret)**:
```cpp
auto [status, sessionKey] = WFX::Hkdf(sharedSecret, salt, "session-v1", 32);
```

---

### Asymmetric (sign/verify)

`AsymKey` is a move-only handle around a public or private key, used for signing and verifying only. There is no RSA-OAEP encryption or ECDH key agreement in this API; if you need that, you're outside what this wrapper covers.

```cpp
enum class CryptoAsymKeyType : std::uint8_t { RSA = 0, EC_P256, EC_P384, ED25519 };

enum class CryptoAsymScheme : std::uint8_t {
    RS256 = 0, RS384, RS512, // RSASSA-PKCS1-v1_5
    PS256, PS384, PS512,     // RSASSA-PSS, salt length == digest length
    ES256, ES384,            // ECDSA P-256/P-384, raw fixed-width R||S
    ED25519                  // Pure EdDSA (RFC 8032), signs the whole message directly
};
```

| Scheme | Digest | Required key type | Signature length |
|---|---|---|---|
| `RS256` / `PS256` | SHA-256 | RSA | modulus size (e.g. 256 bytes for a 2048-bit key) |
| `RS384` / `PS384` | SHA-384 | RSA | modulus size |
| `RS512` / `PS512` | SHA-512 | RSA | modulus size |
| `ES256` | SHA-256 | EC, P-256 curve specifically | 64 bytes, fixed |
| `ES384` | SHA-384 | EC, P-384 curve specifically | 96 bytes, fixed |
| `ED25519` | none, hashed internally | Ed25519 | 64 bytes, fixed |

!!! important
    - Every `Sign`/`Verify` call checks the key's actual type against the requested scheme before touching the signature at all. A mismatch, e.g. `ES256` against an RSA key, or an EC key on the wrong curve, returns `CryptoInvalidArg` immediately instead of producing or accepting a bogus signature.
    - `ES256`/`ES384` signatures are raw fixed-width `R || S` (64 bytes for P-256, 96 for P-384), not OpenSSL's default ASN.1 DER `ECDSA-Sig-Value`. This isn't a JOSE-specific accommodation: RSA signatures here were never DER-wrapped either, and Ed25519 has no DER form anywhere. Raw, fixed-width output is the consistent choice across every scheme, DER is the extra X.509/CMS-era wrapper OpenSSL defaults to.

!!! danger "Do not call `Load`/`Generate` per request"
    RSA key generation is CPU-heavy (hundreds of milliseconds at 2048 bits, worse at 3072/4096), and even PEM/DER parsing in `Load` is not free. Calling either one inside a route handler means paying that cost on every request. Generate or load the key once, e.g. at startup, and hold onto the resulting `AsymKey` (a global, a value captured by a long-lived closure, or a cache keyed by `kid` for the JWKS case, see [JWK](jwk.md)) for every subsequent `Sign`/`Verify` call. `Sign` and `Verify` themselves are cheap enough to call per request once the key already exists.

#### Creating a key

```cpp
static std::pair<CryptoStatus, AsymKey> Load(std::string_view keyData, bool isPrivate);
static std::pair<CryptoStatus, AsymKey> Generate(CryptoAsymKeyType type, std::uint32_t rsaBits = 2048);
static std::pair<CryptoStatus, AsymKey> FromRsaPublic(std::string_view n, std::string_view e);
static std::pair<CryptoStatus, AsymKey> FromEcPublic(CryptoAsymKeyType curve, std::string_view x, std::string_view y);
```

- **`Load`**: parses PEM or DER (auto-detected from the leading bytes), private or public per `isPrivate`. Returns `CryptoInternalError` on any parse failure.
- **`Generate`**: creates a fresh keypair. `rsaBits` only applies to `CryptoAsymKeyType::RSA` (ignored for the other three types) and only `2048`/`3072`/`4096` are accepted; anything else fails the same as a parse error. Returns `CryptoInternalError` on failure, same status as `Load`.
- **`FromRsaPublic`** / **`FromEcPublic`**: build a public-only key straight from raw big-endian integer components (`n`/`e` for RSA, `x`/`y` for EC), no PEM/DER involved at all. This is what you want when a key arrives as raw JWKS fields rather than a certificate or PEM blob. Returns `CryptoInvalidArg` on failure, **not** `CryptoInternalError`, a different status than `Load`/`Generate`, since a malformed field here is a caller input error, not a backend failure.

!!! note
    Every public key this API constructs, via `Load(isPrivate = false)`, `FromRsaPublic`, or `FromEcPublic`, is run through OpenSSL's `EVP_PKEY_public_check` before the handle is considered valid. A structurally invalid public key (e.g. an EC point not actually on the curve) fails construction outright instead of producing a handle that only fails later, at sign/verify time. This matters directly when the key material is attacker-reachable, e.g. sourced from a JWKS endpoint.

**Example (generate once, cache, reuse across requests)**:
```cpp
// Module-scope: paid once at process start, never inside a handler
static WFX::AsymKey g_signingKey = []() {
    auto [status, key] = WFX::AsymKey::Generate(WFX::CryptoEd25519Key);

    // Fail fast at startup
    if(status != WFX::CryptoOk)
        WFX::LogFatal("generate failed");

    return std::move(key);
}();

WFX_POST("/tokens/issue", [](WFX::Request req, WFX::Response res) {
    auto [status, sig] = g_signingKey.Sign(WFX::CryptoEd25519, req.Body());

    if(status != WFX::CryptoOk) {
        res.Status(WFX::HttpStatus::INTERNAL_SERVER_ERROR).SendText("sign failed");
        return;
    }

    res.SendText(WFX::HexEncode({reinterpret_cast<char*>(sig.data()), sig.size()}));
});
```

**Example (loading a configured PEM private key at startup)**:
```cpp
auto [status, key] = WFX::AsymKey::Load(ReadFile("private_key.pem"), /* isPrivate */ true);

if(status != WFX::CryptoOk) {
    // malformed PEM/DER, or the file wasn't actually a private key
}
```

**Example (building a public key straight from JWKS fields)**:
```cpp
// n/e already base64url-decoded, see Encoding and JWK
auto [status, pubKey] = WFX::AsymKey::FromRsaPublic(
    {reinterpret_cast<char*>(n.data()), n.size()},
    {reinterpret_cast<char*>(e.data()), e.size()}
);

if(status != WFX::CryptoOk) {
    // malformed n/e, or the resulting point failed EVP_PKEY_public_check
}
```

#### Signing and verifying

```cpp
std::pair<CryptoStatus, Vector<std::uint8_t>> Sign(CryptoAsymScheme scheme, std::string_view msg) const;
CryptoStatus Verify(CryptoAsymScheme scheme, std::string_view msg, std::string_view sig) const;
```

- **`Sign`**: one-shot only, no streaming variant, for any scheme. Ed25519 in particular cannot be streamed at all (RFC 8032 signs the whole message in a single call), so there's no incremental signing API here regardless of scheme. Requires a private key; `CryptoInvalidArg` if the key/scheme don't match.
- **`Verify`**: `CryptoOk` if the signature verifies, `CryptoAuthFailed` if it's well-formed but cryptographically rejected (tampered message, wrong key, or a signature that just doesn't verify), `CryptoInvalidArg` for a key/scheme mismatch or a malformed signature length (e.g. an `ES256` signature that isn't exactly 64 bytes gets rejected before verification is even attempted).

**Example (verifying an externally-issued signature against a loaded public key)**:
```cpp
auto status = pubKey.Verify(WFX::CryptoRs256, payload, signature);

if(status == WFX::CryptoAuthFailed) {
    res.Status(WFX::HttpStatus::UNAUTHORIZED).SendText("signature rejected");
    return;
}
if(status != WFX::CryptoOk) {
    res.Status(WFX::HttpStatus::BAD_REQUEST).SendText("malformed signature");
    return;
}
```

#### Exporting

```cpp
std::pair<CryptoStatus, Vector<std::uint8_t>> ExportPublic() const;
std::pair<CryptoStatus, Vector<std::uint8_t>> ExportPrivate() const;
```

Both return PEM-encoded key material. `ExportPrivate` returns `CryptoInvalidArg` if the handle only holds public key material, e.g. one built via `FromRsaPublic`/`FromEcPublic`, or `Load`ed with `isPrivate = false`.

**Example (exporting a generated public key for distribution)**:
```cpp
auto [status, pem] = g_signingKey.ExportPublic();
// hand `pem` to whoever needs to verify signatures from this key
```

!!! note
    Loading keys straight out of a JWKS document by `kid`, instead of hand-decoding `n`/`e`/`x`/`y` yourself, is covered by [JWK](jwk.md), which wraps `FromRsaPublic`/`FromEcPublic` for exactly that case.

---

### Misc

#### `RandomBytes`

```cpp
std::pair<CryptoStatus, Vector<std::uint8_t>> RandomBytes(std::uint32_t len);
```

Fills a vector with cryptographically secure random bytes (CSPRNG). `len == 0` returns `CryptoOk` and an empty vector.

#### `ConstantTimeEquals`

```cpp
bool ConstantTimeEquals(std::string_view a, std::string_view b);
```

Timing-safe byte comparison for secrets (MACs, tokens, session IDs). Length is checked first and is not itself treated as secret, since comparing lengths directly before the byte-by-byte comparison is standard practice. Only the equal-length byte comparison runs in constant time.

!!! important
    Never compare secrets with `==`, `std::string_view::compare`, or `memcmp`. All of these can leak timing information proportional to how many leading bytes match. Always use `ConstantTimeEquals` for MACs, tokens, and any other secret comparison.

---

### Full example

```cpp
#include <wfx/http.hpp>
#include <wfx/utils/crypto.hpp>

WFX_POST("/login", [](WFX::Request req, WFX::Response res) {
    auto [status, mac] = WFX::HmacSha256(GetServerSecret(), req.Body());

    if(status != WFX::CryptoOk) {
        res.Status(WFX::HttpStatus::INTERNAL_SERVER_ERROR).SendText("crypto error");
        return;
    }

    auto provided = req.Header("X-Signature");

    if(!WFX::ConstantTimeEquals(mac.View(), provided)) {
        res.Status(WFX::HttpStatus::UNAUTHORIZED).SendText("bad signature");
        return;
    }

    res.SendText("ok");
});
```

---

## Hash

Fast, non-cryptographic hashing for hash tables, cache keys, checksums, and rolling windows. Everything here runs entirely in user code, with no engine ABI call involved, and none of it is safe against an adversary who controls the input. Do not use it for anything security-sensitive; use [Crypto](#crypto) hashing for that instead.

!!! important
    All hash functionality lives directly in the `WFX::` namespace.

    ```cpp
    #include <wfx/utils/hash.hpp>
    ```

### `WyHash`

```cpp
std::uint64_t WyHash(std::string_view data, std::uint64_t seed = 0) noexcept;
```

General-purpose, high-quality, high-throughput hash. The default choice when you just need a good hash of some bytes: hash-map keys, cache keys, deduplication.

### `Fnv1a` / `Fnv1aCaseInsensitive`

```cpp
std::uint64_t Fnv1a(std::string_view data) noexcept;
std::uint64_t Fnv1aCaseInsensitive(std::string_view data) noexcept;
```

FNV-1a. Simpler and slower than `WyHash`, but constexpr-friendly for compile-time string hashing. `Fnv1aCaseInsensitive` folds ASCII case before hashing (`'A'-'Z'` to lowercase), useful for header names and other case-insensitive keys.

### `Xxh3`

```cpp
std::uint64_t Xxh3(std::string_view data, std::uint64_t seed = 0) noexcept;
```

XXH3 (64-bit variant). One of the most widely-deployed non-cryptographic hashes in production infrastructure today (Zstd, LZ4, RocksDB, the Linux kernel), with excellent throughput and avalanche behavior across every input size from a few bytes to gigabytes. Reach for this over `WyHash` when byte-for-byte compatibility with the reference XXH3 algorithm's output matters, e.g. matching hashes with another XXH3 implementation across a network boundary.

### Integer finalizers

```cpp
std::uint32_t Murmur3Mix32(std::uint32_t x) noexcept;
std::uint64_t Murmur3Mix64(std::uint64_t x) noexcept;

std::uint32_t HashInt32(std::uint32_t x) noexcept; // == Murmur3Mix32(x)
std::uint64_t HashInt64(std::uint64_t x) noexcept; // == Murmur3Mix64(x)
```

MurmurHash3's integer finalizer (avalanche mixer). Use these to spread out already-numeric keys (IDs, counters) that would otherwise cluster in a hash table. `HashInt32`/`HashInt64` are plain aliases of the `Murmur3Mix*` functions.

### `HashCombine`

```cpp
std::uint64_t HashCombine(std::uint64_t a, std::uint64_t b) noexcept;
```

Combines two hashes into one (boost::hash_combine-style, 64-bit). Use it to build a composite hash from multiple fields:

```cpp
std::uint64_t h = WFX::HashCombine(WFX::WyHash(name), WFX::HashInt64(id));
```

### `HashValue`

```cpp
template <typename T> std::uint64_t HashValue(const T& val) noexcept;
```

Hashes any trivially copyable value by its raw bytes (`static_assert`s on `std::is_trivially_copyable_v<T>`). Useful for hashing PODs/structs directly without hand-rolling field-by-field combination.
