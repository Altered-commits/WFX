// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_CRYPTO_API_HPP
#define WFX_SHARED_CRYPTO_API_HPP

#include "shared/abis/crypto_types.hpp"

#include <type_traits>

namespace WFX::Shared {

// vvv API declarations vvv
struct CryptoApiExt1 {
    CryptoHashFn hash;
    CryptoHashCreateFn hashCreate;
    CryptoHashUpdateFn hashUpdate;
    CryptoHashFinalFn hashFinal;
    CryptoHashDestroyFn hashDestroy;

    CryptoHmacFn hmac;
    CryptoHmacCreateFn hmacCreate;
    CryptoHmacUpdateFn hmacUpdate;
    CryptoHmacFinalFn hmacFinal;
    CryptoHmacDestroyFn hmacDestroy;

    CryptoAeadEncryptFn aeadEncrypt;
    CryptoAeadDecryptFn aeadDecrypt;

    CryptoPbkdf2Fn pbkdf2;
    CryptoHkdfFn hkdf;
    CryptoArgon2idFn argon2id;

    CryptoRandomBytesFn randomBytes;
    CryptoConstantTimeEqualsFn constantTimeEquals;

    CryptoAsymKeyLoadFn asymKeyLoad;
    CryptoAsymKeyGenerateFn asymKeyGenerate;
    CryptoAsymKeyFromRsaPublicFn asymKeyFromRsaPublic;
    CryptoAsymKeyFromEcPublicFn asymKeyFromEcPublic;
    CryptoAsymKeyPemLenFn asymKeyPemLen;
    CryptoAsymKeyExportFn asymKeyExport;
    CryptoAsymKeyFreeFn asymKeyFree;
    CryptoAsymSigLenFn asymSigLen;
    CryptoAsymSignFn asymSign;
    CryptoAsymVerifyFn asymVerify;
};
static_assert(std::is_standard_layout_v<CryptoApiExt1>, "'CryptoApiExt1' must be standard layout");

// vvv Getter vvv
const CryptoApiExt1* GetCryptoApiExt1();

} // namespace WFX::Shared

#endif // WFX_SHARED_CRYPTO_API_HPP
