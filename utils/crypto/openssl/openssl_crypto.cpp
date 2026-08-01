// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifdef WFX_USE_OPENSSL

#include "openssl_crypto.hpp"
#include "utils/crypto/hash.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/decoder.h>
#include <openssl/ecdsa.h>
#include <openssl/encoder.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/param_build.h>
#include <openssl/params.h>
#include <openssl/rsa.h>

#include <cstring>
#include <memory>
#include <vector>

namespace WFX::Utils::Crypto {

static const EVP_MD* ResolveMd(Shared::CryptoHashAlgo algo)
{
    switch(algo) {
        case Shared::CryptoHashAlgo::SHA256:
            return EVP_sha256();
        case Shared::CryptoHashAlgo::SHA384:
            return EVP_sha384();
        case Shared::CryptoHashAlgo::SHA512:
            return EVP_sha512();
    }

    return nullptr;
}

static const char* HashAlgoName(Shared::CryptoHashAlgo algo)
{
    switch(algo) {
        case Shared::CryptoHashAlgo::SHA256:
            return "SHA256";
        case Shared::CryptoHashAlgo::SHA384:
            return "SHA384";
        case Shared::CryptoHashAlgo::SHA512:
            return "SHA512";
    }

    return nullptr;
}

static const EVP_CIPHER* ResolveAead(Shared::CryptoAeadAlgo algo)
{
    switch(algo) {
        case Shared::CryptoAeadAlgo::AES_256_GCM:
            return EVP_aes_256_gcm();
        case Shared::CryptoAeadAlgo::CHACHA20_POLY1305:
            return EVP_chacha20_poly1305();
    }

    return nullptr;
}

static std::uint32_t AeadKeyLen(Shared::CryptoAeadAlgo algo)
{
    return algo == Shared::CryptoAeadAlgo::AES_256_GCM ? Shared::CRYPTO_AES_GCM_KEY_LEN
                                                       : Shared::CRYPTO_CHA_CHA_KEY_LEN;
}

static std::uint32_t AeadNonceLen(Shared::CryptoAeadAlgo algo)
{
    return algo == Shared::CryptoAeadAlgo::AES_256_GCM ? Shared::CRYPTO_AES_GCM_NONCE_LEN
                                                       : Shared::CRYPTO_CHA_CHA_NONCE_LEN;
}

static std::uint32_t AeadTagLen(Shared::CryptoAeadAlgo algo)
{
    return algo == Shared::CryptoAeadAlgo::AES_256_GCM ? Shared::CRYPTO_AES_GCM_TAG_LEN
                                                       : Shared::CRYPTO_CHA_CHA_TAG_LEN;
}

struct EvpCipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const noexcept
    {
        EVP_CIPHER_CTX_free(ctx);
    }
};
using EvpCipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter>;

// vvv Hashing vvv
Shared::CryptoStatus Hash(Shared::CryptoHashAlgo algo, const std::uint8_t* in, std::uint32_t inLen, std::uint8_t* out,
                          std::uint32_t outCap, std::uint32_t* outLen)
{
    // A null 'in' is only invalid if inLen says otherwise, a null zero-length view is a legitimate empty message
    if((!in && inLen != 0) || !out || !outLen)
        return Shared::CryptoStatus::INVALID_ARG;

    const EVP_MD* md = ResolveMd(algo);
    if(!md)
        return Shared::CryptoStatus::UNSUPPORTED;

    const auto digestLen = static_cast<std::uint32_t>(EVP_MD_size(md));
    if(outCap < digestLen)
        return Shared::CryptoStatus::BUFFER_TOO_SMALL;

    unsigned int written = 0;
    if(EVP_Digest(in, inLen, out, &written, md, nullptr) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    *outLen = written;
    return Shared::CryptoStatus::OK;
}

void* HashCreate(Shared::CryptoHashAlgo algo)
{
    const EVP_MD* md = ResolveMd(algo);
    if(!md)
        return nullptr;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if(!ctx)
        return nullptr;

    if(EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return nullptr;
    }

    return ctx;
}

Shared::CryptoStatus HashUpdate(void* ctx, const std::uint8_t* in, std::uint32_t inLen)
{
    if(!ctx || (!in && inLen != 0))
        return Shared::CryptoStatus::INVALID_ARG;

    if(EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx), in, inLen) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    return Shared::CryptoStatus::OK;
}

Shared::CryptoStatus HashFinal(void* ctx, std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen)
{
    if(!ctx || !out || !outLen)
        return Shared::CryptoStatus::INVALID_ARG;

    auto* mdCtx = static_cast<EVP_MD_CTX*>(ctx);
    const auto digestLen = static_cast<std::uint32_t>(EVP_MD_CTX_get_size(mdCtx));
    if(outCap < digestLen)
        return Shared::CryptoStatus::BUFFER_TOO_SMALL;

    unsigned int written = 0;
    if(EVP_DigestFinal_ex(mdCtx, out, &written) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    *outLen = written;
    return Shared::CryptoStatus::OK;
}

void HashDestroy(void* ctx)
{
    if(ctx)
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx));
}

// vvv HMAC vvv
Shared::CryptoStatus Hmac(Shared::CryptoHashAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                          const std::uint8_t* in, std::uint32_t inLen, std::uint8_t* out, std::uint32_t outCap,
                          std::uint32_t* outLen)
{
    // Null key/in are only invalid if their length says otherwise, an empty key or message is a legitimate HMAC input
    if((!key && keyLen != 0) || (!in && inLen != 0) || !out || !outLen)
        return Shared::CryptoStatus::INVALID_ARG;

    const EVP_MD* md = ResolveMd(algo);
    if(!md)
        return Shared::CryptoStatus::UNSUPPORTED;

    const auto digestLen = static_cast<std::uint32_t>(EVP_MD_size(md));
    if(outCap < digestLen)
        return Shared::CryptoStatus::BUFFER_TOO_SMALL;

    unsigned int written = 0;
    if(HMAC(md, key, static_cast<int>(keyLen), in, inLen, out, &written) == nullptr)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    *outLen = written;
    return Shared::CryptoStatus::OK;
}

void* HmacCreate(Shared::CryptoHashAlgo algo, const std::uint8_t* key, std::uint32_t keyLen)
{
    if(!key && keyLen != 0)
        return nullptr;

    const char* digestName = HashAlgoName(algo);
    if(!digestName)
        return nullptr;

    // Fetched once per call rather than cached: EVP_MAC_fetch is cheap relative to the-
    // -HMAC computation itself, and this keeps ownership local (no process-lifetime global)
    EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if(!mac)
        return nullptr;

    EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac); // EVP_MAC_CTX_new takes its own reference, safe to drop ours now
    if(!ctx)
        return nullptr;

    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string("digest", const_cast<char*>(digestName), 0),
                           OSSL_PARAM_construct_end()};

    if(EVP_MAC_init(ctx, key, keyLen, params) != 1) {
        EVP_MAC_CTX_free(ctx);
        return nullptr;
    }

    return ctx;
}

Shared::CryptoStatus HmacUpdate(void* ctx, const std::uint8_t* in, std::uint32_t inLen)
{
    if(!ctx || (!in && inLen != 0))
        return Shared::CryptoStatus::INVALID_ARG;

    if(EVP_MAC_update(static_cast<EVP_MAC_CTX*>(ctx), in, inLen) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    return Shared::CryptoStatus::OK;
}

Shared::CryptoStatus HmacFinal(void* ctx, std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen)
{
    if(!ctx || !out || !outLen)
        return Shared::CryptoStatus::INVALID_ARG;

    std::size_t written = 0;
    if(EVP_MAC_final(static_cast<EVP_MAC_CTX*>(ctx), out, &written, outCap) != 1)
        return Shared::CryptoStatus::BUFFER_TOO_SMALL;

    *outLen = static_cast<std::uint32_t>(written);
    return Shared::CryptoStatus::OK;
}

void HmacDestroy(void* ctx)
{
    if(ctx)
        EVP_MAC_CTX_free(static_cast<EVP_MAC_CTX*>(ctx));
}

// vvv AEAD vvv
Shared::CryptoStatus AeadEncrypt(Shared::CryptoAeadAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                                 const std::uint8_t* nonce, std::uint32_t nonceLen, const std::uint8_t* aad,
                                 std::uint32_t aadLen, const std::uint8_t* plaintext, std::uint32_t ptLen,
                                 std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen)
{
    // A null plaintext is only invalid if ptLen says otherwise (empty is a legitimate AEAD-
    // -input, e.g. an auth-tag-only token). key/nonce always have fixed non-zero lengths per algo
    if(!key || !nonce || (!plaintext && ptLen != 0) || !out || !outLen)
        return Shared::CryptoStatus::INVALID_ARG;

    const EVP_CIPHER* cipher = ResolveAead(algo);
    if(!cipher)
        return Shared::CryptoStatus::UNSUPPORTED;

    if(keyLen != AeadKeyLen(algo) || nonceLen != AeadNonceLen(algo))
        return Shared::CryptoStatus::INVALID_ARG;

    const std::uint32_t tagLen = AeadTagLen(algo);
    if(outCap < ptLen + tagLen)
        return Shared::CryptoStatus::BUFFER_TOO_SMALL;

    const EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if(!ctx)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    if(EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, key, nonce) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    int len = 0;
    if(aad && aadLen > 0 && EVP_EncryptUpdate(ctx.get(), nullptr, &len, aad, static_cast<int>(aadLen)) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    if(EVP_EncryptUpdate(ctx.get(), out, &len, plaintext, static_cast<int>(ptLen)) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    int ciphertextLen = len;

    if(EVP_EncryptFinal_ex(ctx.get(), out + ciphertextLen, &len) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    ciphertextLen += len;

    if(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, static_cast<int>(tagLen), out + ciphertextLen) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    *outLen = static_cast<std::uint32_t>(ciphertextLen) + tagLen;
    return Shared::CryptoStatus::OK;
}

Shared::CryptoStatus AeadDecrypt(Shared::CryptoAeadAlgo algo, const std::uint8_t* key, std::uint32_t keyLen,
                                 const std::uint8_t* nonce, std::uint32_t nonceLen, const std::uint8_t* aad,
                                 std::uint32_t aadLen, const std::uint8_t* ciphertext, std::uint32_t ctLen,
                                 std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen)
{
    if(!key || !nonce || !ciphertext || !out || !outLen)
        return Shared::CryptoStatus::INVALID_ARG;

    const EVP_CIPHER* cipher = ResolveAead(algo);
    if(!cipher)
        return Shared::CryptoStatus::UNSUPPORTED;

    if(keyLen != AeadKeyLen(algo) || nonceLen != AeadNonceLen(algo))
        return Shared::CryptoStatus::INVALID_ARG;

    const std::uint32_t tagLen = AeadTagLen(algo);
    if(ctLen < tagLen)
        return Shared::CryptoStatus::INVALID_ARG;

    const std::uint32_t actualCtLen = ctLen - tagLen;
    if(outCap < actualCtLen)
        return Shared::CryptoStatus::BUFFER_TOO_SMALL;

    const EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if(!ctx)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    if(EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, key, nonce) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    int len = 0;
    if(aad && aadLen > 0 && EVP_DecryptUpdate(ctx.get(), nullptr, &len, aad, static_cast<int>(aadLen)) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    if(EVP_DecryptUpdate(ctx.get(), out, &len, ciphertext, static_cast<int>(actualCtLen)) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    int plaintextLen = len;

    // OpenSSL takes a non-const void* here despite never writing through it
    auto* tagPtr = const_cast<std::uint8_t*>(ciphertext + actualCtLen);
    if(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(tagLen), tagPtr) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    // EVP_DecryptUpdate above already wrote unauthenticated plaintext into 'out', scrub it-
    // -before returning on tag failure so the caller can't accidentally read it
    if(EVP_DecryptFinal_ex(ctx.get(), out + plaintextLen, &len) != 1) {
        OPENSSL_cleanse(out, actualCtLen);
        return Shared::CryptoStatus::AUTH_FAILED;
    }

    plaintextLen += len;

    *outLen = static_cast<std::uint32_t>(plaintextLen);
    return Shared::CryptoStatus::OK;
}

// vvv Key derivation vvv
Shared::CryptoStatus Pbkdf2(const std::uint8_t* password, std::uint32_t passLen, const std::uint8_t* salt,
                            std::uint32_t saltLen, std::uint32_t iterations, std::uint8_t* out, std::uint32_t outLen)
{
    // Null password/salt/out are only invalid if their length says there's actually data-
    // -behind them, outLen==0 is a no-op (derive zero bytes), not an error
    if((!password && passLen != 0) || (!salt && saltLen != 0) || (!out && outLen != 0))
        return Shared::CryptoStatus::INVALID_ARG;

    if(PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(password), static_cast<int>(passLen), salt,
                         static_cast<int>(saltLen), static_cast<int>(iterations), EVP_sha256(),
                         static_cast<int>(outLen), out) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    return Shared::CryptoStatus::OK;
}

Shared::CryptoStatus Hkdf(const std::uint8_t* ikm, std::uint32_t ikmLen, const std::uint8_t* salt,
                          std::uint32_t saltLen, const std::uint8_t* info, std::uint32_t infoLen, std::uint8_t* out,
                          std::uint32_t outLen)
{
    if((!ikm && ikmLen != 0) || (!out && outLen != 0))
        return Shared::CryptoStatus::INVALID_ARG;

    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    if(!kdf)
        return Shared::CryptoStatus::UNSUPPORTED;

    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if(!ctx)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    char digestName[] = "SHA256";
    std::size_t idx = 0;
    OSSL_PARAM params[5];

    params[idx++] = OSSL_PARAM_construct_utf8_string("digest", digestName, 0);
    params[idx++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, const_cast<std::uint8_t*>(ikm), ikmLen);

    if(salt && saltLen > 0)
        params[idx++] =
            OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<std::uint8_t*>(salt), saltLen);

    if(info && infoLen > 0)
        params[idx++] =
            OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, const_cast<std::uint8_t*>(info), infoLen);

    params[idx++] = OSSL_PARAM_construct_end();

    const Shared::CryptoStatus status =
        EVP_KDF_derive(ctx, out, outLen, params) == 1 ? Shared::CryptoStatus::OK : Shared::CryptoStatus::INTERNAL_ERROR;

    EVP_KDF_CTX_free(ctx);
    return status;
}

Shared::CryptoStatus Argon2id(const std::uint8_t* password, std::uint32_t passLen, const std::uint8_t* salt,
                              std::uint32_t saltLen, std::uint32_t iterations, std::uint32_t memoryKb,
                              std::uint32_t parallelism, std::uint8_t* out, std::uint32_t outLen)
{
    if((!password && passLen != 0) || (!salt && saltLen != 0) || (!out && outLen != 0))
        return Shared::CryptoStatus::INVALID_ARG;

    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if(!kdf)
        return Shared::CryptoStatus::UNSUPPORTED;

    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if(!ctx)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    // OSSL_PARAM_construct_uint32 requires a non-const uint32_t*, so these can't be-
    // -const despite never being written to after initialization
    // NOLINTBEGIN(misc-const-correctness)
    std::uint32_t iter = iterations;
    std::uint32_t mem = memoryKb;
    std::uint32_t lanes = parallelism;
    std::uint32_t threads = parallelism;
    // NOLINTEND(misc-const-correctness)

    OSSL_PARAM params[] = {OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
                                                             const_cast<std::uint8_t*>(password), passLen),
                           OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<std::uint8_t*>(salt),
                                                             saltLen),
                           OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iter),
                           OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &mem),
                           OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
                           OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads),
                           OSSL_PARAM_construct_end()};

    const Shared::CryptoStatus status =
        EVP_KDF_derive(ctx, out, outLen, params) == 1 ? Shared::CryptoStatus::OK : Shared::CryptoStatus::INTERNAL_ERROR;

    EVP_KDF_CTX_free(ctx);
    return status;
}

// vvv Misc vvv
Shared::CryptoStatus RandomBytes(std::uint8_t* out, std::uint32_t len)
{
    // Zero bytes requested is a no-op success, not an error - RandomPool::GetBytes itself rejects-
    // -len==0 (it's a lower-level, SSL-key-focused utility), so this short-circuits before that
    if(len == 0)
        return Shared::CryptoStatus::OK;

    if(!out)
        return Shared::CryptoStatus::INVALID_ARG;

    return GetRandomPool().GetBytes(out, len) ? Shared::CryptoStatus::OK : Shared::CryptoStatus::INTERNAL_ERROR;
}

bool ConstantTimeEquals(const std::uint8_t* a, const std::uint8_t* b, std::uint32_t len)
{
    if((!a && len != 0) || (!b && len != 0))
        return false;

    return CRYPTO_memcmp(a, b, len) == 0;
}

// vvv Asymmetric Crypto vvv
struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const noexcept
    {
        EVP_MD_CTX_free(ctx);
    }
};
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

static const EVP_MD* AsymDigestFor(Shared::CryptoAsymScheme scheme)
{
    switch(scheme) {
        case Shared::CryptoAsymScheme::RS256:
        case Shared::CryptoAsymScheme::PS256:
        case Shared::CryptoAsymScheme::ES256:
            return EVP_sha256();
        case Shared::CryptoAsymScheme::RS384:
        case Shared::CryptoAsymScheme::PS384:
        case Shared::CryptoAsymScheme::ES384:
            return EVP_sha384();
        case Shared::CryptoAsymScheme::RS512:
        case Shared::CryptoAsymScheme::PS512:
            return EVP_sha512();
        case Shared::CryptoAsymScheme::ED25519:
            return nullptr; // EdDSA hashes internally, must not receive an explicit digest
    }

    return nullptr;
}

static bool AsymSchemeIsPss(Shared::CryptoAsymScheme scheme)
{
    return scheme == Shared::CryptoAsymScheme::PS256 || scheme == Shared::CryptoAsymScheme::PS384 ||
           scheme == Shared::CryptoAsymScheme::PS512;
}

static bool AsymSchemeIsEc(Shared::CryptoAsymScheme scheme)
{
    return scheme == Shared::CryptoAsymScheme::ES256 || scheme == Shared::CryptoAsymScheme::ES384;
}

static bool KeyMatchesScheme(EVP_PKEY* key, Shared::CryptoAsymScheme scheme)
{
    const int id = EVP_PKEY_id(key);

    auto isEcGroup = [&](const char* expected) {
        char group[80] = {};
        std::size_t len = 0;
        return id == EVP_PKEY_EC &&
               EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group), &len) == 1 &&
               std::strcmp(group, expected) == 0;
    };

    switch(scheme) {
        case Shared::CryptoAsymScheme::RS256:
        case Shared::CryptoAsymScheme::RS384:
        case Shared::CryptoAsymScheme::RS512:
        case Shared::CryptoAsymScheme::PS256:
        case Shared::CryptoAsymScheme::PS384:
        case Shared::CryptoAsymScheme::PS512:
            return id == EVP_PKEY_RSA;
        case Shared::CryptoAsymScheme::ES256:
            return isEcGroup("prime256v1");
        case Shared::CryptoAsymScheme::ES384:
            return isEcGroup("secp384r1");
        case Shared::CryptoAsymScheme::ED25519:
            return id == EVP_PKEY_ED25519;
    }

    return false;
}

static Shared::CryptoStatus EcdsaDerToRaw(const std::uint8_t* der, long derLen, std::uint32_t rawLen, std::uint8_t* out)
{
    // JWS-style raw fixed-width R||S, converted to/from OpenSSL's native DER ECDSA-Sig-Value
    const std::uint8_t* p = der;
    ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, derLen);
    if(!sig)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(sig, &r, &s);

    const int half = static_cast<int>(rawLen / 2);
    const bool ok = BN_bn2binpad(r, out, half) >= 0 && BN_bn2binpad(s, out + half, half) >= 0;

    ECDSA_SIG_free(sig);
    return ok ? Shared::CryptoStatus::OK : Shared::CryptoStatus::INTERNAL_ERROR;
}

static std::uint8_t* EcdsaRawToDer(const std::uint8_t* raw, std::uint32_t rawLen, int* derLen)
{
    const int half = static_cast<int>(rawLen / 2);
    BIGNUM* r = BN_bin2bn(raw, half, nullptr);
    BIGNUM* s = BN_bin2bn(raw + half, half, nullptr);
    if(!r || !s) {
        BN_free(r);
        BN_free(s);
        return nullptr;
    }

    ECDSA_SIG* sig = ECDSA_SIG_new();
    if(!sig || ECDSA_SIG_set0(sig, r, s) != 1) { // On success 'sig' now owns r/s, freed with it
        BN_free(r);
        BN_free(s);
        if(sig)
            ECDSA_SIG_free(sig);
        return nullptr;
    }

    std::uint8_t* out = nullptr;
    *derLen = i2d_ECDSA_SIG(sig, &out);
    ECDSA_SIG_free(sig);
    return *derLen > 0 ? out : nullptr;
}

static bool PublicKeyValid(EVP_PKEY* pkey)
{
    // Fromdata/PEM/DER parsing doesn't validate the public component on its own. OpenSSL's docs-
    // -call this out as required for externally-sourced keys, which covers every key built here
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_pkey(nullptr, pkey, nullptr);
    if(!ctx)
        return false;

    const bool valid = EVP_PKEY_public_check(ctx) == 1;
    EVP_PKEY_CTX_free(ctx);
    return valid;
}

void* AsymKeyLoad(const std::uint8_t* keyData, std::uint32_t keyLen, bool isPrivate)
{
    if(!keyData || keyLen == 0)
        return nullptr;

    // format/structure left null so the decoder auto-detects PEM vs DER on its own, no manual-
    // -header sniffing needed
    EVP_PKEY* pkey = nullptr;
    const int selection = isPrivate ? EVP_PKEY_PRIVATE_KEY : EVP_PKEY_PUBLIC_KEY;
    OSSL_DECODER_CTX* dctx =
        OSSL_DECODER_CTX_new_for_pkey(&pkey, nullptr, nullptr, nullptr, selection, nullptr, nullptr);
    if(!dctx)
        return nullptr;

    const std::uint8_t* data = keyData;
    std::size_t dataLen = keyLen;
    const bool ok = OSSL_DECODER_from_data(dctx, &data, &dataLen) == 1;
    OSSL_DECODER_CTX_free(dctx);

    if(!ok) {
        EVP_PKEY_free(pkey);
        return nullptr;
    }

    if(!isPrivate && !PublicKeyValid(pkey)) {
        EVP_PKEY_free(pkey);
        return nullptr;
    }

    return pkey;
}

void* AsymKeyGenerate(Shared::CryptoAsymKeyType type, std::uint32_t rsaBits)
{
    switch(type) {
        case Shared::CryptoAsymKeyType::RSA:
            if(rsaBits != 2048 && rsaBits != 3072 && rsaBits != 4096)
                return nullptr;
            return EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<std::size_t>(rsaBits));
        case Shared::CryptoAsymKeyType::EC_P256:
            return EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
        case Shared::CryptoAsymKeyType::EC_P384:
            return EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-384");
        case Shared::CryptoAsymKeyType::ED25519:
            return EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    }

    return nullptr;
}

// Builds an EVP_PKEY straight from raw params via the OSSL_PARAM_BLD path, no PEM/DER involved
static EVP_PKEY* PkeyFromParams(const char* keyType, OSSL_PARAM_BLD* bld)
{
    OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
    if(!params)
        return nullptr;

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_from_name(nullptr, keyType, nullptr);
    EVP_PKEY* pkey = nullptr;

    if(pctx && EVP_PKEY_fromdata_init(pctx) == 1)
        EVP_PKEY_fromdata(pctx, &pkey, EVP_PKEY_PUBLIC_KEY, params);

    EVP_PKEY_CTX_free(pctx);
    OSSL_PARAM_free(params);

    if(pkey && !PublicKeyValid(pkey)) {
        EVP_PKEY_free(pkey);
        return nullptr;
    }

    return pkey;
}

void* AsymKeyFromRsaPublic(const std::uint8_t* n, std::uint32_t nLen, const std::uint8_t* e, std::uint32_t eLen)
{
    if(!n || nLen == 0 || !e || eLen == 0)
        return nullptr;

    BIGNUM* bnN = BN_bin2bn(n, static_cast<int>(nLen), nullptr);
    BIGNUM* bnE = BN_bin2bn(e, static_cast<int>(eLen), nullptr);

    EVP_PKEY* pkey = nullptr;
    if(bnN && bnE) {
        OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
        if(bld && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bnN) &&
           OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, bnE))
            pkey = PkeyFromParams("RSA", bld);

        OSSL_PARAM_BLD_free(bld);
    }

    BN_free(bnN);
    BN_free(bnE);
    return pkey;
}

void* AsymKeyFromEcPublic(Shared::CryptoAsymKeyType curve, const std::uint8_t* x, std::uint32_t xLen,
                          const std::uint8_t* y, std::uint32_t yLen)
{
    if(!x || xLen == 0 || !y || yLen == 0)
        return nullptr;

    const char* groupName = curve == Shared::CryptoAsymKeyType::EC_P256   ? "prime256v1"
                            : curve == Shared::CryptoAsymKeyType::EC_P384 ? "secp384r1"
                                                                          : nullptr;
    if(!groupName)
        return nullptr;

    // Uncompressed SEC1 point format: 0x04 || X || Y
    std::vector<std::uint8_t> point;
    point.reserve(1 + xLen + yLen);
    point.push_back(0x04);
    point.insert(point.end(), x, x + xLen);
    point.insert(point.end(), y, y + yLen);

    EVP_PKEY* pkey = nullptr;
    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    if(bld && OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, groupName, 0) &&
       OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()))
        pkey = PkeyFromParams("EC", bld);

    OSSL_PARAM_BLD_free(bld);
    return pkey;
}

static bool EncodePem(EVP_PKEY* pkey, bool exportPrivate, unsigned char** outData, std::size_t* outLen)
{
    // Encodes straight into a freshly OPENSSL_zalloc'd buffer, no BIO involved. Caller frees the-
    // -returned buffer with OPENSSL_free(). False on failure (e.g. exportPrivate against a public-
    // -only handle)
    const int selection = exportPrivate ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY;
    const char* structure = exportPrivate ? "PrivateKeyInfo" : "SubjectPublicKeyInfo";

    OSSL_ENCODER_CTX* ectx = OSSL_ENCODER_CTX_new_for_pkey(pkey, selection, "PEM", structure, nullptr);
    if(!ectx)
        return false;

    *outData = nullptr;
    *outLen = 0;
    const bool ok = OSSL_ENCODER_to_data(ectx, outData, outLen) == 1;

    OSSL_ENCODER_CTX_free(ectx);
    return ok;
}

std::uint32_t AsymKeyPemLen(void* key, bool exportPrivate)
{
    if(!key)
        return 0;

    unsigned char* data = nullptr;
    std::size_t len = 0;
    if(!EncodePem(static_cast<EVP_PKEY*>(key), exportPrivate, &data, &len))
        return 0;

    OPENSSL_free(data);
    return static_cast<std::uint32_t>(len);
}

Shared::CryptoStatus AsymKeyExport(void* key, bool exportPrivate, std::uint8_t* out, std::uint32_t outCap,
                                   std::uint32_t* outLen)
{
    if(!key || !out || !outLen)
        return Shared::CryptoStatus::INVALID_ARG;

    unsigned char* data = nullptr;
    std::size_t len = 0;

    if(!EncodePem(static_cast<EVP_PKEY*>(key), exportPrivate, &data, &len))
        return exportPrivate ? Shared::CryptoStatus::INVALID_ARG : Shared::CryptoStatus::INTERNAL_ERROR;

    Shared::CryptoStatus status = Shared::CryptoStatus::OK;

    if(static_cast<std::uint32_t>(len) > outCap)
        status = Shared::CryptoStatus::BUFFER_TOO_SMALL;
    else {
        std::memcpy(out, data, len);
        *outLen = static_cast<std::uint32_t>(len);
    }

    OPENSSL_free(data);
    return status;
}

void AsymKeyFree(void* key)
{
    if(key)
        EVP_PKEY_free(static_cast<EVP_PKEY*>(key));
}

std::uint32_t AsymSigLen(void* key, Shared::CryptoAsymScheme scheme)
{
    if(!key)
        return 0;

    auto* pkey = static_cast<EVP_PKEY*>(key);
    if(!KeyMatchesScheme(pkey, scheme))
        return 0;

    if(AsymSchemeIsEc(scheme))
        return scheme == Shared::CryptoAsymScheme::ES256 ? Shared::CRYPTO_ECDSA_P256_SIG_LEN
                                                         : Shared::CRYPTO_ECDSA_P384_SIG_LEN;

    if(scheme == Shared::CryptoAsymScheme::ED25519)
        return Shared::CRYPTO_ED25519_SIG_LEN;

    return static_cast<std::uint32_t>(EVP_PKEY_size(pkey)); // RSA: exact signature length for this key
}

Shared::CryptoStatus AsymSign(void* privKey, Shared::CryptoAsymScheme scheme, const std::uint8_t* msg,
                              std::uint32_t msgLen, std::uint8_t* out, std::uint32_t outCap, std::uint32_t* outLen)
{
    if(!privKey || (!msg && msgLen != 0) || !out || !outLen)
        return Shared::CryptoStatus::INVALID_ARG;

    auto* pkey = static_cast<EVP_PKEY*>(privKey);
    if(!KeyMatchesScheme(pkey, scheme))
        return Shared::CryptoStatus::INVALID_ARG;

    const EvpMdCtxPtr ctx(EVP_MD_CTX_new());
    if(!ctx)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    EVP_PKEY_CTX* pctx = nullptr;
    if(EVP_DigestSignInit(ctx.get(), &pctx, AsymDigestFor(scheme), nullptr, pkey) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    if(AsymSchemeIsPss(scheme) && (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
                                   EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1))
        return Shared::CryptoStatus::INTERNAL_ERROR;

    // First call with a null buffer returns the required signature length, DER upper-bound for EC
    std::size_t sigLen = 0;
    if(EVP_DigestSign(ctx.get(), nullptr, &sigLen, msg, msgLen) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    std::vector<std::uint8_t> sigBuf(sigLen);
    if(EVP_DigestSign(ctx.get(), sigBuf.data(), &sigLen, msg, msgLen) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    if(AsymSchemeIsEc(scheme)) {
        const std::uint32_t rawLen = scheme == Shared::CryptoAsymScheme::ES256 ? Shared::CRYPTO_ECDSA_P256_SIG_LEN
                                                                               : Shared::CRYPTO_ECDSA_P384_SIG_LEN;
        if(outCap < rawLen)
            return Shared::CryptoStatus::BUFFER_TOO_SMALL;

        const Shared::CryptoStatus status = EcdsaDerToRaw(sigBuf.data(), static_cast<long>(sigLen), rawLen, out);
        if(status == Shared::CryptoStatus::OK)
            *outLen = rawLen;

        return status;
    }

    if(outCap < sigLen)
        return Shared::CryptoStatus::BUFFER_TOO_SMALL;

    std::memcpy(out, sigBuf.data(), sigLen);
    *outLen = static_cast<std::uint32_t>(sigLen);

    return Shared::CryptoStatus::OK;
}

Shared::CryptoStatus AsymVerify(void* pubKey, Shared::CryptoAsymScheme scheme, const std::uint8_t* msg,
                                std::uint32_t msgLen, const std::uint8_t* sig, std::uint32_t sigLen)
{
    if(!pubKey || (!msg && msgLen != 0) || !sig || sigLen == 0)
        return Shared::CryptoStatus::INVALID_ARG;

    auto* pkey = static_cast<EVP_PKEY*>(pubKey);
    if(!KeyMatchesScheme(pkey, scheme))
        return Shared::CryptoStatus::INVALID_ARG;

    const EvpMdCtxPtr ctx(EVP_MD_CTX_new());
    if(!ctx)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    EVP_PKEY_CTX* pctx = nullptr;
    if(EVP_DigestVerifyInit(ctx.get(), &pctx, AsymDigestFor(scheme), nullptr, pkey) != 1)
        return Shared::CryptoStatus::INTERNAL_ERROR;

    if(AsymSchemeIsPss(scheme) && (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
                                   EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1))
        return Shared::CryptoStatus::INTERNAL_ERROR;

    if(AsymSchemeIsEc(scheme)) {
        const std::uint32_t expected = scheme == Shared::CryptoAsymScheme::ES256 ? Shared::CRYPTO_ECDSA_P256_SIG_LEN
                                                                                 : Shared::CRYPTO_ECDSA_P384_SIG_LEN;
        if(sigLen != expected)
            return Shared::CryptoStatus::INVALID_ARG;

        int derLen = 0;
        std::uint8_t* der = EcdsaRawToDer(sig, sigLen, &derLen);
        if(!der)
            return Shared::CryptoStatus::INTERNAL_ERROR;

        const int rc = EVP_DigestVerify(ctx.get(), der, static_cast<std::size_t>(derLen), msg, msgLen);
        OPENSSL_free(der);
        return rc == 1 ? Shared::CryptoStatus::OK : Shared::CryptoStatus::AUTH_FAILED;
    }

    const int rc = EVP_DigestVerify(ctx.get(), sig, sigLen, msg, msgLen);
    return rc == 1 ? Shared::CryptoStatus::OK : Shared::CryptoStatus::AUTH_FAILED;
}

} // namespace WFX::Utils::Crypto

#endif // WFX_USE_OPENSSL
