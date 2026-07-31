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
    AUTH_FAILED,      // AEAD tag or asymmetric signature verification failed
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

// ChaCha20-Poly1305 (RFC 8439, 12B nonce). Prefer a counter over pure randomness at high volume
inline constexpr std::uint32_t CRYPTO_CHA_CHA_KEY_LEN = 32;
inline constexpr std::uint32_t CRYPTO_CHA_CHA_NONCE_LEN = 12;
inline constexpr std::uint32_t CRYPTO_CHA_CHA_TAG_LEN = 16;

// vvv One-shot hashing vvv
// Digest of [in,inLen) into out. outCap must be >= the algo's digest length (32/48/64 for-
// -SHA256/384/512), and *outLen receives the actual length written
using CryptoHashFn = CryptoStatus (*)(CryptoHashAlgo algo, const std::uint8_t* in, std::uint32_t inLen,
                                      std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);

// vvv Streaming hashing (for data too large to buffer in one call) vvv
using CryptoHashCreateFn = void* (*)(CryptoHashAlgo algo);
using CryptoHashUpdateFn = CryptoStatus (*)(void* ctx, const std::uint8_t* in, std::uint32_t inLen);
using CryptoHashFinalFn = CryptoStatus (*)(void* ctx, std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);
using CryptoHashDestroyFn = void (*)(void* ctx);

// vvv One-shot HMAC vvv
using CryptoHmacFn = CryptoStatus (*)(CryptoHashAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                                      const std::uint8_t* in, std::uint32_t inLen, std::uint8_t* out,
                                      std::uint32_t outCap, std::uint32_t* outLen);

// vvv Streaming HMAC vvv
using CryptoHmacCreateFn = void* (*)(CryptoHashAlgo algo, const std::uint8_t* key, std::uint32_t keyLen);
using CryptoHmacUpdateFn = CryptoStatus (*)(void* ctx, const std::uint8_t* in, std::uint32_t inLen);
using CryptoHmacFinalFn = CryptoStatus (*)(void* ctx, std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);
using CryptoHmacDestroyFn = void (*)(void* ctx);

// vvv AEAD vvv
// Writes ciphertext followed by the auth tag into out (outCap must be >= ptLen + the algo's-
// -tag length), and *outLen receives the total bytes written
using CryptoAeadEncryptFn = CryptoStatus (*)(CryptoAeadAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                                             const std::uint8_t* nonce, std::uint32_t nonceLen, const std::uint8_t* aad,
                                             std::uint32_t aadLen, const std::uint8_t* plaintext, std::uint32_t ptLen,
                                             std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);

// ciphertext must include the trailing tag. Returns AUTH_FAILED (out left untouched) on tag-
// -mismatch, never releases unauthenticated plaintext
using CryptoAeadDecryptFn = CryptoStatus (*)(CryptoAeadAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                                             const std::uint8_t* nonce, std::uint32_t nonceLen, const std::uint8_t* aad,
                                             std::uint32_t aadLen, const std::uint8_t* ciphertext, std::uint32_t ctLen,
                                             std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);

// vvv Key derivation vvv
// PBKDF2-HMAC-SHA256 (RFC 8018), writes exactly outLen bytes
using CryptoPbkdf2Fn = CryptoStatus (*)(const std::uint8_t* password, std::uint32_t passLen, const std::uint8_t* salt,
                                        std::uint32_t saltLen, std::uint32_t iterations, std::uint8_t* out,
                                        std::uint32_t outLen);

// HKDF-SHA256, extract-then-expand (RFC 5869), writes exactly outLen bytes
using CryptoHkdfFn = CryptoStatus (*)(const std::uint8_t* ikm, std::uint32_t ikmLen, const std::uint8_t* salt,
                                      std::uint32_t saltLen, const std::uint8_t* info, std::uint32_t infoLen,
                                      std::uint8_t* out, std::uint32_t outLen);

// Argon2id password hashing (RFC 9106), writes exactly outLen bytes. memoryKb is the m_cost-
// -parameter in KiB, iterations is t_cost, parallelism is the lane count. Returns UNSUPPORTED-
// -if the backend's Argon2 provider isn't available
using CryptoArgon2idFn = CryptoStatus (*)(const std::uint8_t* password, std::uint32_t passLen, const std::uint8_t* salt,
                                          std::uint32_t saltLen, std::uint32_t iterations, std::uint32_t memoryKb,
                                          std::uint32_t parallelism, std::uint8_t* out, std::uint32_t outLen);

// vvv Misc vvv
using CryptoRandomBytesFn = CryptoStatus (*)(std::uint8_t* out, std::uint32_t len);
using CryptoConstantTimeEqualsFn = bool (*)(const std::uint8_t* a, const std::uint8_t* b, std::uint32_t len);

// vvv Asymmetric Crypto vvv
enum class CryptoAsymKeyType : std::uint8_t { RSA = 0, EC_P256, EC_P384, ED25519 };
enum class CryptoAsymScheme : std::uint8_t {
    RS256 = 0, RS384, RS512, // RSASSA-PKCS1-v1_5
    PS256, PS384, PS512,     // RSASSA-PSS, salt length == digest length
    ES256, ES384,            // ECDSA P-256/P-384, raw fixed-width R||S, not ASN.1 DER
    ED25519                  // Pure EdDSA (RFC 8032), signs the whole message directly
};

// Fixed signature lengths (bytes). RSA schemes have none here, theirs is always rsaBits/8
inline constexpr std::uint32_t CRYPTO_ECDSA_P256_SIG_LEN = 64;
inline constexpr std::uint32_t CRYPTO_ECDSA_P384_SIG_LEN = 96;
inline constexpr std::uint32_t CRYPTO_ED25519_SIG_LEN = 64;

// Parses PEM or DER (auto-detected), private or public per isPrivate. Free with CryptoAsymKeyFreeFn
using CryptoAsymKeyLoadFn = void* (*)(const std::uint8_t* keyData, std::uint32_t keyLen, bool isPrivate);

// rsaBits only applies to CryptoAsymKeyType::RSA, and only 2048/3072/4096 are accepted
using CryptoAsymKeyGenerateFn = void* (*)(CryptoAsymKeyType type, std::uint32_t rsaBits);

// Builds a public-only key straight from raw big-endian integer components, no PEM/DER involved
using CryptoAsymKeyFromRsaPublicFn = void* (*)(const std::uint8_t* n, std::uint32_t nLen, const std::uint8_t* e,
                                               std::uint32_t eLen);
using CryptoAsymKeyFromEcPublicFn = void* (*)(CryptoAsymKeyType curve, const std::uint8_t* x, std::uint32_t xLen,
                                              const std::uint8_t* y, std::uint32_t yLen);

// Exact PEM size a matching AsymKeyExportFn call would need, 0 on failure
using CryptoAsymKeyPemLenFn = std::uint32_t (*)(void* key, bool exportPrivate);

// exportPrivate=true fails with INVALID_ARG if the handle only holds public key material
using CryptoAsymKeyExportFn = CryptoStatus (*)(void* key, bool exportPrivate, std::uint8_t* out,
                                               std::uint32_t outCap, std::uint32_t* outLen);
using CryptoAsymKeyFreeFn = void (*)(void* key);

// Exact signature size for this key+scheme pair, so a caller can size a buffer without guessing-
// -0 if key and scheme don't match
using CryptoAsymSigLenFn = std::uint32_t (*)(void* key, CryptoAsymScheme scheme);

// One-shot only, Ed25519 has no incremental signing mode
using CryptoAsymSignFn = CryptoStatus (*)(void* privKey, CryptoAsymScheme scheme, const std::uint8_t* msg,
                                          std::uint32_t msgLen, std::uint8_t* out, std::uint32_t outCap,
                                          std::uint32_t* outLen);

// OK = verified, AUTH_FAILED = signature rejected, INVALID_ARG = key/scheme mismatch
using CryptoAsymVerifyFn = CryptoStatus (*)(void* pubKey, CryptoAsymScheme scheme, const std::uint8_t* msg,
                                            std::uint32_t msgLen, const std::uint8_t* sig, std::uint32_t sigLen);

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_CRYPTO_TYPES_HPP
