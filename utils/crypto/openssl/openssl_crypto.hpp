// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifdef WFX_USE_OPENSSL

#ifndef WFX_UTILS_OPENSSL_CRYPTO_HPP
#define WFX_UTILS_OPENSSL_CRYPTO_HPP

// Stateless free functions, backend choice is a compile-time #ifdef in shared/apis/crypto_api.cpp
#include "shared/abis/crypto_types.hpp"

namespace WFX::Utils::Crypto {

// vvv Hashing vvv
Shared::CryptoStatus Hash(Shared::CryptoHashAlgo algo, const std::uint8_t* in, std::uint32_t inLen, std::uint8_t* out,
                          std::uint32_t outCap, std::uint32_t* outLen);

void* HashCreate(Shared::CryptoHashAlgo algo);
Shared::CryptoStatus HashUpdate(void* ctx, const std::uint8_t* in, std::uint32_t inLen);
Shared::CryptoStatus HashFinal(void* ctx, std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);
void HashDestroy(void* ctx);

// vvv HMAC vvv
Shared::CryptoStatus Hmac(Shared::CryptoHashAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                          const std::uint8_t* in, std::uint32_t inLen, std::uint8_t* out, std::uint32_t outCap,
                          std::uint32_t* outLen);

void* HmacCreate(Shared::CryptoHashAlgo algo, const std::uint8_t* key, std::uint32_t keyLen);
Shared::CryptoStatus HmacUpdate(void* ctx, const std::uint8_t* in, std::uint32_t inLen);
Shared::CryptoStatus HmacFinal(void* ctx, std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);
void HmacDestroy(void* ctx);

// vvv AEAD (one-shot only) vvv
Shared::CryptoStatus AeadEncrypt(Shared::CryptoAeadAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                                 const std::uint8_t* nonce, std::uint32_t nonceLen, const std::uint8_t* aad,
                                 std::uint32_t aadLen, const std::uint8_t* plaintext, std::uint32_t ptLen,
                                 std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);
Shared::CryptoStatus AeadDecrypt(Shared::CryptoAeadAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                                 const std::uint8_t* nonce, std::uint32_t nonceLen, const std::uint8_t* aad,
                                 std::uint32_t aadLen, const std::uint8_t* ciphertext, std::uint32_t ctLen,
                                 std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);

// vvv Key derivation vvv
Shared::CryptoStatus Pbkdf2(const std::uint8_t* password, std::uint32_t passLen, const std::uint8_t* salt,
                            std::uint32_t saltLen, std::uint32_t iterations, std::uint8_t* out, std::uint32_t outLen);
Shared::CryptoStatus Hkdf(const std::uint8_t* ikm, std::uint32_t ikmLen, const std::uint8_t* salt,
                          std::uint32_t saltLen, const std::uint8_t* info, std::uint32_t infoLen, std::uint8_t* out,
                          std::uint32_t outLen);
Shared::CryptoStatus Argon2id(const std::uint8_t* password, std::uint32_t passLen, const std::uint8_t* salt,
                              std::uint32_t saltLen, std::uint32_t iterations, std::uint32_t memoryKb,
                              std::uint32_t parallelism, std::uint8_t* out, std::uint32_t outLen);

// vvv Misc vvv
// NOTE: does NOT go through OpenSSL - WFX already has a kernel-backed CSPRNG
// (Utils::RandomPool, getrandom()/dev-urandom) used for SSL key generation, and this
// delegates to that instead of duplicating via OpenSSL's RAND_bytes, so there's exactly
// one source of randomness in the engine.
Shared::CryptoStatus RandomBytes(std::uint8_t* out, std::uint32_t len);
bool ConstantTimeEquals(const std::uint8_t* a, const std::uint8_t* b, std::uint32_t len);

// vvv Asymmetric vvv
void* AsymKeyLoad(const std::uint8_t* keyData, std::uint32_t keyLen, bool isPrivate);
void* AsymKeyGenerate(Shared::CryptoAsymKeyType type, std::uint32_t rsaBits);
void* AsymKeyFromRsaPublic(const std::uint8_t* n, std::uint32_t nLen, const std::uint8_t* e, std::uint32_t eLen);
void* AsymKeyFromEcPublic(Shared::CryptoAsymKeyType curve, const std::uint8_t* x, std::uint32_t xLen,
                          const std::uint8_t* y, std::uint32_t yLen);
std::uint32_t AsymKeyPemLen(void* key, bool exportPrivate);
Shared::CryptoStatus AsymKeyExport(void* key, bool exportPrivate, std::uint8_t* out, std::uint32_t outCap,
                                   std::uint32_t* outLen);
void AsymKeyFree(void* key);

std::uint32_t AsymSigLen(void* key, Shared::CryptoAsymScheme scheme);
Shared::CryptoStatus AsymSign(void* privKey, Shared::CryptoAsymScheme scheme, const std::uint8_t* msg,
                              std::uint32_t msgLen, std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen);
Shared::CryptoStatus AsymVerify(void* pubKey, Shared::CryptoAsymScheme scheme, const std::uint8_t* msg,
                                std::uint32_t msgLen, const std::uint8_t* sig, std::uint32_t sigLen);

} // namespace WFX::Utils::Crypto

#endif // WFX_UTILS_OPENSSL_CRYPTO_HPP

#endif // WFX_USE_OPENSSL
