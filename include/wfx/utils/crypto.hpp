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
//
// Every function returns {CryptoStatus, result}; check status == WFX::CryptoOk-
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

using Shared::CryptoAeadAlgo;
using Shared::CryptoHashAlgo;
using Shared::CryptoStatus;

inline constexpr auto CryptoOk = CryptoStatus::OK;
inline constexpr auto CryptoAesGcm = CryptoAeadAlgo::AES_256_GCM;
inline constexpr auto CryptoChaCha20Poly1305 = CryptoAeadAlgo::CHACHA20_POLY1305;

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
            return CryptoStatus::INVALID_ARG;

        return Core::CryptoApiExt1()->hashUpdate(ctx_, reinterpret_cast<const std::uint8_t*>(data.data()),
                                                 static_cast<std::uint32_t>(data.size()));
    }

    std::pair<CryptoStatus, Digest<DigestLenFor<Algo>()>> Final()
    {
        Digest<DigestLenFor<Algo>()> d;
        if(!ctx_)
            return {CryptoStatus::INVALID_ARG, d};

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
            return CryptoStatus::INVALID_ARG;

        return Core::CryptoApiExt1()->hmacUpdate(ctx_, reinterpret_cast<const std::uint8_t*>(data.data()),
                                                 static_cast<std::uint32_t>(data.size()));
    }

    std::pair<CryptoStatus, Digest<DigestLenFor<Algo>()>> Final()
    {
        Digest<DigestLenFor<Algo>()> d;
        if(!ctx_)
            return {CryptoStatus::INVALID_ARG, d};

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
        return {CryptoStatus::INVALID_ARG, {}};

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
        return {CryptoStatus::INVALID_ARG, {}};

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
// -for MAC/token checks); only the byte-by-byte comparison of equal-length secrets-
// -needs to be timing-safe
inline bool ConstantTimeEquals(std::string_view a, std::string_view b)
{
    if(a.size() != b.size())
        return false;

    return Core::CryptoApiExt1()->constantTimeEquals(reinterpret_cast<const std::uint8_t*>(a.data()),
                                                     reinterpret_cast<const std::uint8_t*>(b.data()),
                                                     static_cast<std::uint32_t>(a.size()));
}

} // namespace WFX

#endif // WFX_INC_WFX_UTILS_CRYPTO_HPP
