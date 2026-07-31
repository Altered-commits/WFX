// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_UTILS_CRYPTO_HPP
#define WFX_INC_WFX_UTILS_CRYPTO_HPP

// -----------------------------------------------------------------------
// wfx/utils/crypto.hpp
// User-facing wrappers over the engine's crypto backend (OpenSSL today).
// Every call crosses into the engine via Core::CryptoApiExt1()
//
// Provides:
//   WFX::Sha256/384/512(data)                  : one-shot hashing
//   WFX::HashStream<Algo>                      : incremental hashing for large data
//   WFX::HmacSha256/384/512(key, data)         : one-shot HMAC
//   WFX::HmacStream<Algo>                      : incremental HMAC for large data
//   WFX::AeadEncrypt/AeadDecrypt(...)          : AES-256-GCM / ChaCha20-Poly1305
//   WFX::Pbkdf2/Hkdf/Argon2id(...)             : key derivation
//   WFX::RandomBytes(len)                      : CSPRNG
//   WFX::ConstantTimeEquals(a, b)              : timing-safe comparison for secrets
//   WFX::AsymKey                               : asymmetric key handle, sign/verify
//
// Every function returns {CryptoStatus, result}. Check status == WFX::CryptoOk-
// -before using the result.
// -----------------------------------------------------------------------

#include "core/core.hpp"
#include "shared/abis/crypto_types.hpp"
#include "wfx/memory.hpp"

#include <array>
#include <string_view>
#include <utility>
#include <vector>

namespace WFX {

using CryptoAeadAlgo = Shared::CryptoAeadAlgo;
using CryptoAsymKeyType = Shared::CryptoAsymKeyType;
using CryptoAsymScheme = Shared::CryptoAsymScheme;
using CryptoHashAlgo = Shared::CryptoHashAlgo;
using CryptoStatus = Shared::CryptoStatus;

inline constexpr auto CryptoOk = CryptoStatus::OK;
inline constexpr auto CryptoInvalidArg = CryptoStatus::INVALID_ARG;
inline constexpr auto CryptoBufferTooSmall = CryptoStatus::BUFFER_TOO_SMALL;
inline constexpr auto CryptoAuthFailed = CryptoStatus::AUTH_FAILED;
inline constexpr auto CryptoUnsupported = CryptoStatus::UNSUPPORTED;
inline constexpr auto CryptoInternalError = CryptoStatus::INTERNAL_ERROR;

inline constexpr auto CryptoAesGcm = CryptoAeadAlgo::AES_256_GCM;
inline constexpr auto CryptoChaCha20Poly1305 = CryptoAeadAlgo::CHACHA20_POLY1305;

inline constexpr auto CryptoRsaKey = CryptoAsymKeyType::RSA;
inline constexpr auto CryptoEcP256Key = CryptoAsymKeyType::EC_P256;
inline constexpr auto CryptoEcP384Key = CryptoAsymKeyType::EC_P384;
inline constexpr auto CryptoEd25519Key = CryptoAsymKeyType::ED25519;

inline constexpr auto CryptoRs256 = CryptoAsymScheme::RS256;
inline constexpr auto CryptoRs384 = CryptoAsymScheme::RS384;
inline constexpr auto CryptoRs512 = CryptoAsymScheme::RS512;
inline constexpr auto CryptoPs256 = CryptoAsymScheme::PS256;
inline constexpr auto CryptoPs384 = CryptoAsymScheme::PS384;
inline constexpr auto CryptoPs512 = CryptoAsymScheme::PS512;
inline constexpr auto CryptoEs256 = CryptoAsymScheme::ES256;
inline constexpr auto CryptoEs384 = CryptoAsymScheme::ES384;
inline constexpr auto CryptoEd25519 = CryptoAsymScheme::ED25519;

// vvv Hashing / HMAC result vvv
// N is the digest length for the algo that produced it (32/48/64 for SHA256/384/512)
template <std::uint32_t N> struct Digest {
    std::array<std::uint8_t, N> bytes{};
    std::uint32_t len = 0;

    std::string_view View() const noexcept
    {
        return {reinterpret_cast<const char*>(bytes.data()), len};
    }
};

template <CryptoHashAlgo Algo> inline constexpr std::uint32_t DigestLenFor()
{
    if constexpr(Algo == CryptoHashAlgo::SHA256)
        return Shared::CRYPTO_SHA256_LEN;
    else if constexpr(Algo == CryptoHashAlgo::SHA384)
        return Shared::CRYPTO_SHA384_LEN;
    else
        return Shared::CRYPTO_SHA512_LEN;
}

// vvv One-shot hashing vvv
template <CryptoHashAlgo Algo> inline std::pair<CryptoStatus, Digest<DigestLenFor<Algo>()>> Hash(std::string_view data)
{
    Digest<DigestLenFor<Algo>()> d;
    const CryptoStatus status = Core::CryptoApiExt1()->hash(Algo, reinterpret_cast<const std::uint8_t*>(data.data()),
                                                            static_cast<std::uint32_t>(data.size()), d.bytes.data(),
                                                            static_cast<std::uint32_t>(d.bytes.size()), &d.len);

    return {status, d};
}

inline auto Sha256(std::string_view data)
{
    return Hash<CryptoHashAlgo::SHA256>(data);
}
inline auto Sha384(std::string_view data)
{
    return Hash<CryptoHashAlgo::SHA384>(data);
}
inline auto Sha512(std::string_view data)
{
    return Hash<CryptoHashAlgo::SHA512>(data);
}

// vvv Streaming hashing, for data too large to buffer in one call vvv
template <CryptoHashAlgo Algo> class HashStream {
public: // Constructor and Destructor
    HashStream() : ctx_(Core::CryptoApiExt1()->hashCreate(Algo))
    {}
    ~HashStream()
    {
        if(ctx_)
            Core::CryptoApiExt1()->hashDestroy(ctx_);
    }

    HashStream(const HashStream&) = delete;
    HashStream& operator=(const HashStream&) = delete;

    HashStream(HashStream&& other) noexcept : ctx_(other.ctx_)
    {
        other.ctx_ = nullptr;
    }
    HashStream& operator=(HashStream&& other) noexcept
    {
        if(this != &other) {
            if(ctx_)
                Core::CryptoApiExt1()->hashDestroy(ctx_);

            ctx_ = other.ctx_;
            other.ctx_ = nullptr;
        }

        return *this;
    }

public: // Main functions
    bool Valid() const noexcept
    {
        return ctx_ != nullptr;
    }

    CryptoStatus Update(std::string_view data)
    {
        if(!ctx_)
            return CryptoInvalidArg;

        return Core::CryptoApiExt1()->hashUpdate(ctx_, reinterpret_cast<const std::uint8_t*>(data.data()),
                                                 static_cast<std::uint32_t>(data.size()));
    }

    std::pair<CryptoStatus, Digest<DigestLenFor<Algo>()>> Final()
    {
        Digest<DigestLenFor<Algo>()> d;
        if(!ctx_)
            return {CryptoInvalidArg, d};

        const CryptoStatus status =
            Core::CryptoApiExt1()->hashFinal(ctx_, d.bytes.data(), static_cast<std::uint32_t>(d.bytes.size()), &d.len);

        return {status, d};
    }

private: // Storage
    void* ctx_;
};

// vvv One-shot HMAC vvv
template <CryptoHashAlgo Algo>
inline std::pair<CryptoStatus, Digest<DigestLenFor<Algo>()>> Hmac(std::string_view key, std::string_view data)
{
    Digest<DigestLenFor<Algo>()> d;
    const CryptoStatus status = Core::CryptoApiExt1()->hmac(Algo, reinterpret_cast<const std::uint8_t*>(key.data()),
                                                            static_cast<std::uint32_t>(key.size()),
                                                            reinterpret_cast<const std::uint8_t*>(data.data()),
                                                            static_cast<std::uint32_t>(data.size()), d.bytes.data(),
                                                            static_cast<std::uint32_t>(d.bytes.size()), &d.len);
    return {status, d};
}

inline auto HmacSha256(std::string_view key, std::string_view data)
{
    return Hmac<CryptoHashAlgo::SHA256>(key, data);
}
inline auto HmacSha384(std::string_view key, std::string_view data)
{
    return Hmac<CryptoHashAlgo::SHA384>(key, data);
}
inline auto HmacSha512(std::string_view key, std::string_view data)
{
    return Hmac<CryptoHashAlgo::SHA512>(key, data);
}

// vvv Streaming HMAC, for data too large to buffer in one call vvv
template <CryptoHashAlgo Algo> class HmacStream {
public: // Constructor and Destructor
    explicit HmacStream(std::string_view key)
        : ctx_(Core::CryptoApiExt1()->hmacCreate(Algo, reinterpret_cast<const std::uint8_t*>(key.data()),
                                                 static_cast<std::uint32_t>(key.size())))
    {}
    ~HmacStream()
    {
        if(ctx_)
            Core::CryptoApiExt1()->hmacDestroy(ctx_);
    }

    HmacStream(const HmacStream&) = delete;
    HmacStream& operator=(const HmacStream&) = delete;

    HmacStream(HmacStream&& other) noexcept : ctx_(other.ctx_)
    {
        other.ctx_ = nullptr;
    }
    HmacStream& operator=(HmacStream&& other) noexcept
    {
        if(this != &other) {
            if(ctx_)
                Core::CryptoApiExt1()->hmacDestroy(ctx_);

            ctx_ = other.ctx_;
            other.ctx_ = nullptr;
        }

        return *this;
    }

public: // Main functions
    bool Valid() const noexcept
    {
        return ctx_ != nullptr;
    }

    CryptoStatus Update(std::string_view data)
    {
        if(!ctx_)
            return CryptoInvalidArg;

        return Core::CryptoApiExt1()->hmacUpdate(ctx_, reinterpret_cast<const std::uint8_t*>(data.data()),
                                                 static_cast<std::uint32_t>(data.size()));
    }

    std::pair<CryptoStatus, Digest<DigestLenFor<Algo>()>> Final()
    {
        Digest<DigestLenFor<Algo>()> d;
        if(!ctx_)
            return {CryptoInvalidArg, d};

        const CryptoStatus status =
            Core::CryptoApiExt1()->hmacFinal(ctx_, d.bytes.data(), static_cast<std::uint32_t>(d.bytes.size()), &d.len);

        return {status, d};
    }

private: // Storage
    void* ctx_;
};

// vvv AEAD vvv
// One-shot only (see crypto_types.hpp for why). Input and output both live in memory at-
// -once, so this is not the right tool for large payloads regardless of the cap below -
// -real large-data support needs chunked, independently-authenticated framing (libsodium's-
// -crypto_secretstream is the standard example), not a bigger buffer. The cap exists to-
// -fail loudly on an accidentally-huge body instead of silently costing 2x its RAM
inline constexpr std::size_t CryptoAeadMaxSize = 64 * 1024 * 1024;

inline std::pair<CryptoStatus, Vector<std::uint8_t>> AeadEncrypt(CryptoAeadAlgo algo, std::string_view key,
                                                                 std::string_view nonce, std::string_view aad,
                                                                 std::string_view plaintext)
{
    if(plaintext.size() > CryptoAeadMaxSize)
        return {CryptoInvalidArg, {}};

    const std::uint32_t tagLen =
        (algo == CryptoAesGcm) ? Shared::CRYPTO_AES_GCM_TAG_LEN : Shared::CRYPTO_CHA_CHA_TAG_LEN;

    Vector<std::uint8_t> out(plaintext.size() + tagLen);

    std::uint32_t written = 0;
    const CryptoStatus status =
        Core::CryptoApiExt1()->aeadEncrypt(algo, reinterpret_cast<const std::uint8_t*>(key.data()),
                                           static_cast<std::uint32_t>(key.size()),
                                           reinterpret_cast<const std::uint8_t*>(nonce.data()),
                                           static_cast<std::uint32_t>(nonce.size()),
                                           reinterpret_cast<const std::uint8_t*>(aad.data()),
                                           static_cast<std::uint32_t>(aad.size()),
                                           reinterpret_cast<const std::uint8_t*>(plaintext.data()),
                                           static_cast<std::uint32_t>(plaintext.size()), out.data(),
                                           static_cast<std::uint32_t>(out.size()), &written);

    if(status == CryptoOk)
        out.resize(written);
    else
        out.clear();

    return {status, std::move(out)};
}

inline std::pair<CryptoStatus, Vector<std::uint8_t>> AeadDecrypt(CryptoAeadAlgo algo, std::string_view key,
                                                                 std::string_view nonce, std::string_view aad,
                                                                 std::string_view ciphertext)
{
    if(ciphertext.size() > CryptoAeadMaxSize)
        return {CryptoInvalidArg, {}};

    Vector<std::uint8_t> out(ciphertext.size()); // plaintext is always shorter than this

    std::uint32_t written = 0;
    const CryptoStatus status =
        Core::CryptoApiExt1()->aeadDecrypt(algo, reinterpret_cast<const std::uint8_t*>(key.data()),
                                           static_cast<std::uint32_t>(key.size()),
                                           reinterpret_cast<const std::uint8_t*>(nonce.data()),
                                           static_cast<std::uint32_t>(nonce.size()),
                                           reinterpret_cast<const std::uint8_t*>(aad.data()),
                                           static_cast<std::uint32_t>(aad.size()),
                                           reinterpret_cast<const std::uint8_t*>(ciphertext.data()),
                                           static_cast<std::uint32_t>(ciphertext.size()), out.data(),
                                           static_cast<std::uint32_t>(out.size()), &written);

    if(status == CryptoOk)
        out.resize(written);
    else
        out.clear(); // never expose partial/unauthenticated plaintext on failure

    return {status, std::move(out)};
}

// vvv Key derivation vvv
inline std::pair<CryptoStatus, Vector<std::uint8_t>> Pbkdf2(std::string_view password, std::string_view salt,
                                                            std::uint32_t iterations, std::uint32_t outLen)
{
    Vector<std::uint8_t> out(outLen);
    const CryptoStatus status =
        Core::CryptoApiExt1()->pbkdf2(reinterpret_cast<const std::uint8_t*>(password.data()),
                                      static_cast<std::uint32_t>(password.size()),
                                      reinterpret_cast<const std::uint8_t*>(salt.data()),
                                      static_cast<std::uint32_t>(salt.size()), iterations, out.data(), outLen);

    if(status != CryptoOk)
        out.clear();

    return {status, std::move(out)};
}

inline std::pair<CryptoStatus, Vector<std::uint8_t>> Hkdf(std::string_view ikm, std::string_view salt,
                                                          std::string_view info, std::uint32_t outLen)
{
    Vector<std::uint8_t> out(outLen);
    const CryptoStatus status =
        Core::CryptoApiExt1()->hkdf(reinterpret_cast<const std::uint8_t*>(ikm.data()),
                                    static_cast<std::uint32_t>(ikm.size()),
                                    reinterpret_cast<const std::uint8_t*>(salt.data()),
                                    static_cast<std::uint32_t>(salt.size()),
                                    reinterpret_cast<const std::uint8_t*>(info.data()),
                                    static_cast<std::uint32_t>(info.size()), out.data(), outLen);

    if(status != CryptoOk)
        out.clear();

    return {status, std::move(out)};
}

inline std::pair<CryptoStatus, Vector<std::uint8_t>> Argon2id(std::string_view password, std::string_view salt,
                                                              std::uint32_t iterations, std::uint32_t memoryKb,
                                                              std::uint32_t parallelism, std::uint32_t outLen)
{
    Vector<std::uint8_t> out(outLen);
    const CryptoStatus status = Core::CryptoApiExt1()->argon2id(reinterpret_cast<const std::uint8_t*>(password.data()),
                                                                static_cast<std::uint32_t>(password.size()),
                                                                reinterpret_cast<const std::uint8_t*>(salt.data()),
                                                                static_cast<std::uint32_t>(salt.size()), iterations,
                                                                memoryKb, parallelism, out.data(), outLen);

    if(status != CryptoOk)
        out.clear();

    return {status, std::move(out)};
}

// vvv Misc vvv
inline std::pair<CryptoStatus, Vector<std::uint8_t>> RandomBytes(std::uint32_t len)
{
    Vector<std::uint8_t> out(len);
    const CryptoStatus status = Core::CryptoApiExt1()->randomBytes(out.data(), len);

    if(status != CryptoOk)
        out.clear();

    return {status, std::move(out)};
}

// Length itself isn't treated as secret (comparing it directly is standard practice-
// -for MAC/token checks). Only the byte-by-byte comparison of equal-length secrets-
// -needs to be timing-safe
inline bool ConstantTimeEquals(std::string_view a, std::string_view b)
{
    if(a.size() != b.size())
        return false;

    return Core::CryptoApiExt1()->constantTimeEquals(reinterpret_cast<const std::uint8_t*>(a.data()),
                                                     reinterpret_cast<const std::uint8_t*>(b.data()),
                                                     static_cast<std::uint32_t>(a.size()));
}

// vvv Asymmetric vvv
class AsymKey {
public: // Constructor and Destructor
    AsymKey() = default;
    ~AsymKey()
    {
        if(ctx_)
            Core::CryptoApiExt1()->asymKeyFree(ctx_);
    }

    AsymKey(const AsymKey&) = delete;
    AsymKey& operator=(const AsymKey&) = delete;

    AsymKey(AsymKey&& other) noexcept : ctx_(other.ctx_)
    {
        other.ctx_ = nullptr;
    }
    AsymKey& operator=(AsymKey&& other) noexcept
    {
        if(this != &other) {
            if(ctx_)
                Core::CryptoApiExt1()->asymKeyFree(ctx_);

            ctx_ = other.ctx_;
            other.ctx_ = nullptr;
        }

        return *this;
    }

public: // Main functions
    static std::pair<CryptoStatus, AsymKey> Load(std::string_view keyData, bool isPrivate)
    {
        AsymKey key;
        key.ctx_ = Core::CryptoApiExt1()->asymKeyLoad(reinterpret_cast<const std::uint8_t*>(keyData.data()),
                                                      static_cast<std::uint32_t>(keyData.size()), isPrivate);

        return {key.ctx_ ? CryptoOk : CryptoInternalError, std::move(key)};
    }

    static std::pair<CryptoStatus, AsymKey> Generate(CryptoAsymKeyType type, std::uint32_t rsaBits = 2048)
    {
        AsymKey key;
        key.ctx_ = Core::CryptoApiExt1()->asymKeyGenerate(type, rsaBits);

        return {key.ctx_ ? CryptoOk : CryptoInternalError, std::move(key)};
    }

    // Public-only, built straight from raw big-endian integer components, no PEM/DER involved
    static std::pair<CryptoStatus, AsymKey> FromRsaPublic(std::string_view n, std::string_view e)
    {
        AsymKey key;
        key.ctx_ = Core::CryptoApiExt1()->asymKeyFromRsaPublic(reinterpret_cast<const std::uint8_t*>(n.data()),
                                                               static_cast<std::uint32_t>(n.size()),
                                                               reinterpret_cast<const std::uint8_t*>(e.data()),
                                                               static_cast<std::uint32_t>(e.size()));

        return {key.ctx_ ? CryptoOk : CryptoInvalidArg, std::move(key)};
    }

    static std::pair<CryptoStatus, AsymKey> FromEcPublic(CryptoAsymKeyType curve, std::string_view x,
                                                         std::string_view y)
    {
        AsymKey key;
        key.ctx_ = Core::CryptoApiExt1()->asymKeyFromEcPublic(curve, reinterpret_cast<const std::uint8_t*>(x.data()),
                                                              static_cast<std::uint32_t>(x.size()),
                                                              reinterpret_cast<const std::uint8_t*>(y.data()),
                                                              static_cast<std::uint32_t>(y.size()));

        return {key.ctx_ ? CryptoOk : CryptoInvalidArg, std::move(key)};
    }

    bool Valid() const noexcept
    {
        return ctx_ != nullptr;
    }

    std::pair<CryptoStatus, Vector<std::uint8_t>> ExportPublic() const
    {
        return Export(false);
    }

    std::pair<CryptoStatus, Vector<std::uint8_t>> ExportPrivate() const
    {
        return Export(true);
    }

    std::pair<CryptoStatus, Vector<std::uint8_t>> Sign(CryptoAsymScheme scheme, std::string_view msg) const
    {
        if(!ctx_)
            return {CryptoInvalidArg, {}};

        const std::uint32_t sigLen = Core::CryptoApiExt1()->asymSigLen(ctx_, scheme);
        if(sigLen == 0)
            return {CryptoInvalidArg, {}};

        Vector<std::uint8_t> out(sigLen);
        std::uint32_t written = 0;
        const CryptoStatus status =
            Core::CryptoApiExt1()->asymSign(ctx_, scheme, reinterpret_cast<const std::uint8_t*>(msg.data()),
                                            static_cast<std::uint32_t>(msg.size()), out.data(),
                                            static_cast<std::uint32_t>(out.size()), &written);

        if(status == CryptoOk)
            out.resize(written);
        else
            out.clear();

        return {status, std::move(out)};
    }

    CryptoStatus Verify(CryptoAsymScheme scheme, std::string_view msg, std::string_view sig) const
    {
        if(!ctx_)
            return CryptoInvalidArg;

        return Core::CryptoApiExt1()->asymVerify(ctx_, scheme, reinterpret_cast<const std::uint8_t*>(msg.data()),
                                                 static_cast<std::uint32_t>(msg.size()),
                                                 reinterpret_cast<const std::uint8_t*>(sig.data()),
                                                 static_cast<std::uint32_t>(sig.size()));
    }

private: // Helper functions
    std::pair<CryptoStatus, Vector<std::uint8_t>> Export(bool exportPrivate) const
    {
        if(!ctx_)
            return {CryptoInvalidArg, {}};

        const std::uint32_t pemLen = Core::CryptoApiExt1()->asymKeyPemLen(ctx_, exportPrivate);
        if(pemLen == 0)
            return {CryptoInvalidArg, {}};

        Vector<std::uint8_t> out(pemLen);
        std::uint32_t written = 0;
        const CryptoStatus status = Core::CryptoApiExt1()->asymKeyExport(
            ctx_, exportPrivate, out.data(), static_cast<std::uint32_t>(out.size()), &written);

        if(status == CryptoOk)
            out.resize(written);
        else
            out.clear();

        return {status, std::move(out)};
    }

private: // Storage
    void* ctx_ = nullptr;
};

} // namespace WFX

#endif // WFX_INC_WFX_UTILS_CRYPTO_HPP
