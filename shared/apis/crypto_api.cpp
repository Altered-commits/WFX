// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "crypto_api.hpp"

#ifdef WFX_USE_OPENSSL
#include "utils/crypto/openssl/openssl_crypto.hpp"
#else
#error "WFX_USE_OPENSSL macro not found. Only OpenSSL is supported for now"
#endif

namespace WFX::Shared {

namespace CryptoImpl = WFX::Utils::Crypto;

const CryptoApiExt1* GetCryptoApiExt1()
{
    // clang-format off
    // NOLINTNEXTLINE(readability-identifier-naming) - singleton table, treated as Global variable
    static const CryptoApiExt1 GlobalCryptoAPIExt1 = {
        CryptoImpl::Hash,
        CryptoImpl::HashCreate,
        CryptoImpl::HashUpdate,
        CryptoImpl::HashFinal,
        CryptoImpl::HashDestroy,

        CryptoImpl::Hmac,
        CryptoImpl::HmacCreate,
        CryptoImpl::HmacUpdate,
        CryptoImpl::HmacFinal,
        CryptoImpl::HmacDestroy,

        CryptoImpl::AeadEncrypt,
        CryptoImpl::AeadDecrypt,

        CryptoImpl::Pbkdf2,
        CryptoImpl::Hkdf,
        CryptoImpl::Argon2id,

        CryptoImpl::RandomBytes,
        CryptoImpl::ConstantTimeEquals,

        CryptoImpl::AsymKeyLoad,
        CryptoImpl::AsymKeyGenerate,
        CryptoImpl::AsymKeyFromRsaPublic,
        CryptoImpl::AsymKeyFromEcPublic,
        CryptoImpl::AsymKeyPemLen,
        CryptoImpl::AsymKeyExport,
        CryptoImpl::AsymKeyFree,
        CryptoImpl::AsymSigLen,
        CryptoImpl::AsymSign,
        CryptoImpl::AsymVerify,
    };
    // clang-format on

    return &GlobalCryptoAPIExt1;
}

} // namespace WFX::Shared
