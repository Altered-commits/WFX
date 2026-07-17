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

### `Djb2`

```cpp
std::uint32_t Djb2(std::string_view data) noexcept;
```

Classic DJB2, 32-bit output. Simple, constexpr-friendly, adequate for small-scale internal use.

### `Adler32`

```cpp
std::uint32_t Adler32(std::string_view data) noexcept;
```

Adler-32 checksum. Fast, but weak for short inputs. Prefer it for large-payload integrity checks, not as a general hash-map key.

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

### `RollingHash`

```cpp
struct RollingHash {
    void Push(std::uint8_t byte) noexcept;
    void Roll(std::uint8_t outgoing, std::uint8_t incoming) noexcept;
    std::uint64_t Value() const noexcept;
};
```

Incremental window hash (Rabin-Karp style, modulo a Mersenne prime) for algorithms that need a hash of a sliding window over a byte stream without rehashing the whole window each time.

- `Push(byte)`: grow the window by one byte. Use this while first building the window.
- `Roll(outgoing, incoming)`: slide the window by one byte, dropping `outgoing` from the front and adding `incoming` to the back.
- `Value()`: current hash of the window.

**Example (fixed-size sliding window)**:
```cpp
WFX::RollingHash rh;

// Build the initial window
for(std::size_t i = 0; i < windowSize; i++)
    rh.Push(data[i]);

auto firstWindowHash = rh.Value();

// Slide forward one byte at a time
for(std::size_t i = windowSize; i < data.size(); i++) {
    rh.Roll(data[i - windowSize], data[i]);
    auto hash = rh.Value();
    // ...
}
```

!!! note
    `RollingHash` does not track the window size or contents for you. `Roll()` trusts you to pass the byte actually leaving the window as `outgoing`. Track the window boundaries yourself, e.g. with a ring buffer or index pair.
