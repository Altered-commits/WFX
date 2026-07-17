// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_ABI_CRYPTO_TYPES_HPP
#define WFX_SHARED_ABI_CRYPTO_TYPES_HPP

#include <cstdint>

namespace WFX::Shared {

enum class CryptoHashAlgo : std::uint8_t { SHA256 = 0, SHA384, SHA512 };
enum class CryptoAeadAlgo : std::uint8_t { AES_256_GCM = 0, CHACHA20_POLY1305 };

enum class CryptoStatus : std::uint8_t {
    OK = 0,
    INVALID_ARG,      // Null pointer, or key/nonce/output length doesn't match the algo
    BUFFER_TOO_SMALL, // Caller-provided output buffer is too small
    AUTH_FAILED,      // AEAD tag verification failed on decrypt; out is left untouched
    UNSUPPORTED,      // Backend doesn't provide this algorithm (e.g. Argon2 provider missing)
    INTERNAL_ERROR    // Underlying crypto library failure
};

// Digest lengths (bytes), also the HMAC output length for the same algo
inline constexpr std::uint32_t CRYPTO_SHA256_LEN = 32;
inline constexpr std::uint32_t CRYPTO_SHA384_LEN = 48;
inline constexpr std::uint32_t CRYPTO_SHA512_LEN = 64;

// AES-256-GCM: 32B key, 12B nonce (IETF-standard), 16B tag appended to ciphertext
inline constexpr std::uint32_t CRYPTO_AES_GCM_KEY_LEN = 32;
inline constexpr std::uint32_t CRYPTO_AES_GCM_NONCE_LEN = 12;
inline constexpr std::uint32_t CRYPTO_AES_GCM_TAG_LEN = 16;

// ChaCha20-Poly1305 (RFC 8439, standard 12B nonce, NOT the 24B-nonce XChaCha20 variant,-
// -which OpenSSL doesn't expose as a ready-made cipher). Random nonces are only safe up to-
// -~2^32 messages under one key before collision risk becomes non-negligible; high-volume-
// -callers should use a counter-based nonce instead of pure randomness (same caveat as-
// -AES-256-GCM above)
inline constexpr std::uint32_t CRYPTO_CHA_CHA_KEY_LEN = 32;
inline constexpr std::uint32_t CRYPTO_CHA_CHA_NONCE_LEN = 12;
inline constexpr std::uint32_t CRYPTO_CHA_CHA_TAG_LEN = 16;

// vvv One-shot hashing vvv
// Digest of [in,inLen) into out; outCap must be >= the algo's digest length (32/48/64 for-
// -SHA256/384/512); *outLen receives the actual length written
using CryptoHashFn = CryptoStatus (*)(CryptoHashAlgo algo, const std::uint8_t* in, std::uint32_t inLen,
                                      std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);

// vvv Streaming hashing (for data too large to buffer in one call) vvv
using CryptoHashCreateFn = void* (*)(CryptoHashAlgo algo);
using CryptoHashUpdateFn = CryptoStatus (*)(void* ctx, const std::uint8_t* in, std::uint32_t inLen);
using CryptoHashFinalFn = CryptoStatus (*)(void* ctx, std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);
using CryptoHashDestroyFn = void (*)(void* ctx);

// vvv One-shot HMAC vvv
// HMAC of [in,inLen) under key; outCap/*outLen as CryptoHashFn (digest length of algo)
using CryptoHmacFn = CryptoStatus (*)(CryptoHashAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                                      const std::uint8_t* in, std::uint32_t inLen, std::uint8_t* out,
                                      std::uint32_t outCap, std::uint32_t* outLen);

// vvv Streaming HMAC vvv
using CryptoHmacCreateFn = void* (*)(CryptoHashAlgo algo, const std::uint8_t* key, std::uint32_t keyLen);
using CryptoHmacUpdateFn = CryptoStatus (*)(void* ctx, const std::uint8_t* in, std::uint32_t inLen);
using CryptoHmacFinalFn = CryptoStatus (*)(void* ctx, std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);
using CryptoHmacDestroyFn = void (*)(void* ctx);

// vvv AEAD (one-shot only; a streaming AEAD decrypt can't release plaintext before its-
// -tag is verified, which needs a chunked-tag construction, not just a bigger buffer) vvv
// Writes ciphertext followed by the auth tag into out (outCap must be >= ptLen + the algo's-
// -tag length); *outLen receives the total bytes written
using CryptoAeadEncryptFn = CryptoStatus (*)(CryptoAeadAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                                             const std::uint8_t* nonce, std::uint32_t nonceLen, const std::uint8_t* aad,
                                             std::uint32_t aadLen, const std::uint8_t* plaintext, std::uint32_t ptLen,
                                             std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);
// ciphertext must include the trailing tag; returns AUTH_FAILED (out left untouched) on tag-
// -mismatch, never releases unauthenticated plaintext
using CryptoAeadDecryptFn = CryptoStatus (*)(CryptoAeadAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                                             const std::uint8_t* nonce, std::uint32_t nonceLen, const std::uint8_t* aad,
                                             std::uint32_t aadLen, const std::uint8_t* ciphertext, std::uint32_t ctLen,
                                             std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);

// vvv Key derivation vvv
// PBKDF2-HMAC-SHA256 (RFC 8018); writes exactly outLen bytes
using CryptoPbkdf2Fn = CryptoStatus (*)(const std::uint8_t* password, std::uint32_t passLen, const std::uint8_t* salt,
                                        std::uint32_t saltLen, std::uint32_t iterations, std::uint8_t* out,
                                        std::uint32_t outLen);
// HKDF-SHA256, extract-then-expand (RFC 5869); writes exactly outLen bytes
using CryptoHkdfFn = CryptoStatus (*)(const std::uint8_t* ikm, std::uint32_t ikmLen, const std::uint8_t* salt,
                                      std::uint32_t saltLen, const std::uint8_t* info, std::uint32_t infoLen,
                                      std::uint8_t* out, std::uint32_t outLen);
// Argon2id password hashing (RFC 9106); writes exactly outLen bytes. memoryKb is the m_cost-
// -parameter in KiB, iterations is t_cost, parallelism is the lane count. Returns UNSUPPORTED-
// -if the backend's Argon2 provider isn't available
using CryptoArgon2idFn = CryptoStatus (*)(const std::uint8_t* password, std::uint32_t passLen, const std::uint8_t* salt,
                                          std::uint32_t saltLen, std::uint32_t iterations, std::uint32_t memoryKb,
                                          std::uint32_t parallelism, std::uint8_t* out, std::uint32_t outLen);

// vvv Misc vvv
// Fills out with cryptographically secure random bytes (CSPRNG)
using CryptoRandomBytesFn = CryptoStatus (*)(std::uint8_t* out, std::uint32_t len);
// Constant-time comparison, safe for comparing secrets (MACs, tokens) without leaking-
// -timing information; true iff equal
using CryptoConstantTimeEqualsFn = bool (*)(const std::uint8_t* a, const std::uint8_t* b, std::uint32_t len);

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_CRYPTO_TYPES_HPP
