// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_UTILS_JWK_HPP
#define WFX_INC_WFX_UTILS_JWK_HPP

// -----------------------------------------------------------------------
// wfx/utils/jwk.hpp
// Turns a JWKS JSON body (RFC 7517, e.g. Cloudflare Access's /cdn-cgi/access/certs) into an-
// -AsymKey by kid. Kept separate from wfx/utils/crypto.hpp - AsymKey is generic asym crypto,-
// -this is the JOSE-specific layer built on top of it
//
// Provides:
//   WFX::LoadJwk(jwksJson, kid)   : {CryptoStatus, AsymKey}, public key only
// -----------------------------------------------------------------------

#include "wfx/http.hpp"
#include "wfx/utils/crypto.hpp"
#include "wfx/utils/encoding.hpp"

#include <string_view>
#include <utility>

namespace WFX {

inline std::pair<CryptoStatus, AsymKey> LoadJwk(std::string_view jwksJson, std::string_view kid)
{
    auto result = ParseJson(jwksJson);
    if(!result.IsValid())
        return {CryptoInvalidArg, AsymKey()};

    auto keys = result.object.Get("keys");
    if(!keys.IsArray())
        return {CryptoInvalidArg, AsymKey()};

    for(std::uint32_t i = 0; i < keys.Length(); ++i) {
        auto key = keys[i];
        if(key.Get("kid").AsString() != kid)
            continue;

        const auto kty = key.Get("kty").AsString();

        if(kty == "RSA") {
            auto [nOk, n] = Base64Decode(key.Get("n").AsString());
            auto [eOk, e] = Base64Decode(key.Get("e").AsString());
            if(!nOk || !eOk)
                return {CryptoInvalidArg, AsymKey()};

            return AsymKey::FromRsaPublic({reinterpret_cast<const char*>(n.data()), n.size()},
                                          {reinterpret_cast<const char*>(e.data()), e.size()});
        }

        if(kty == "EC") {
            const auto crv = key.Get("crv").AsString();
            CryptoAsymKeyType curve{};
            if(crv == "P-256")
                curve = CryptoEcP256Key;
            else if(crv == "P-384")
                curve = CryptoEcP384Key;
            else
                return {CryptoUnsupported, AsymKey()};

            auto [xOk, x] = Base64Decode(key.Get("x").AsString());
            auto [yOk, y] = Base64Decode(key.Get("y").AsString());
            if(!xOk || !yOk)
                return {CryptoInvalidArg, AsymKey()};

            return AsymKey::FromEcPublic(curve, {reinterpret_cast<const char*>(x.data()), x.size()},
                                         {reinterpret_cast<const char*>(y.data()), y.size()});
        }

        return {CryptoUnsupported, AsymKey()};
    }

    return {CryptoInvalidArg, AsymKey()}; // kid not found
}

} // namespace WFX

#endif // WFX_INC_WFX_UTILS_JWK_HPP
