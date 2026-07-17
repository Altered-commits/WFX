// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifdef WFX_USE_OPENSSL

#include "openssl_crypto.hpp"
#include "utils/crypto/hash.hpp"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <memory>

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
    // A null 'in' is only invalid if inLen says there's actually data behind it - a null,
    // zero-length view (e.g. an empty std::string_view) is a legitimate empty message
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
    // Null key/in are only invalid if their length says there's actually data behind them -
    // an empty key or empty message are both legitimate (if unusual) HMAC inputs
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
    // A null plaintext is only invalid if ptLen says there's actually data behind it - an
    // empty message is a legitimate AEAD input (e.g. an auth-tag-only token). key/nonce have
    // fixed required lengths per algo (never legitimately 0), so they stay unconditionally
    // null-checked
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

    // EVP_DecryptUpdate above already wrote unauthenticated plaintext into 'out'; scrub it-
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
    // -behind them; outLen==0 is a no-op (derive zero bytes), not an error
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
    // Zero bytes requested is a no-op success, not an error - RandomPool::GetBytes itself
    // hard-rejects len==0 (it's a lower-level, SSL-key-focused utility), so short-circuit
    // here rather than treating that as a failure
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

} // namespace WFX::Utils::Crypto

#endif // WFX_USE_OPENSSL
