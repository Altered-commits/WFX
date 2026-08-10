// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_UTILS_JWT_HPP
#define WFX_INC_WFX_UTILS_JWT_HPP

// -----------------------------------------------------------------------
// wfx/utils/jwt.hpp
// Compact JWT (RFC 7519) parsing, standard claim checks, and signature
// verification. Kept separate from wfx/utils/jwk.hpp: that file turns a
// JWKS body into an AsymKey by kid, this file is what you do with a token
// once you already have the right key
//
// Provides:
//   WFX::ParseJwt(token)                          : {CryptoStatus, JwtParts}
//   WFX::JwtTimeClaimsValid(payload)               : exp/nbf check
//   WFX::JwtAudienceMatches(payload, expectedAud)  : aud check, string or array form
//   WFX::VerifyJwtSignature(parts, key)            : dispatches on parts.alg
//
// Fetching or caching the signing key itself (e.g. polling a JWKS endpoint)
// is out of scope here, same as wfx/utils/jwk.hpp, that always depends on a
// specific provider's endpoint and HTTP client, left to the caller
// -----------------------------------------------------------------------

#include "wfx/types.hpp"
#include "wfx/utils/crypto.hpp"
#include "wfx/utils/encoding.hpp"

#include <chrono>
#include <string_view>
#include <utility>

namespace WFX {

struct JwtParts {
    JsonObject header;
    JsonObject payload;
    Vector<std::uint8_t> signature;
    std::string_view signingInput; // header + "." + payload, exactly as it appeared on the wire
    std::string_view alg;          // from the header, e.g. "RS256". Empty if absent
    std::string_view kid;          // from the header. Empty if absent
};

// Splits, base64url-decodes, and JSON-parses a compact "header.payload.signature" token.
// Does not verify the signature or check any claims, see VerifyJwtSignature and the
// JwtTimeClaimsValid/JwtAudienceMatches functions below for that
inline std::pair<CryptoStatus, JwtParts> ParseJwt(std::string_view token)
{
    JwtParts parts;

    const auto dot1 = token.find('.');
    const auto dot2 = token.find('.', dot1 == std::string_view::npos ? 0 : dot1 + 1);
    if(dot1 == std::string_view::npos || dot2 == std::string_view::npos)
        return {CryptoInvalidArg, std::move(parts)};

    // The signature covers header+payload exactly as they appeared on the wire, before decoding
    parts.signingInput = token.substr(0, dot2);

    auto [hdrOk, headerBytes] = Base64Decode(token.substr(0, dot1));
    auto [payloadOk, payloadBytes] = Base64Decode(token.substr(dot1 + 1, dot2 - dot1 - 1));
    auto [sigOk, sig] = Base64Decode(token.substr(dot2 + 1));

    if(!hdrOk || !payloadOk || !sigOk)
        return {CryptoInvalidArg, std::move(parts)};

    auto header = ParseJson({reinterpret_cast<char*>(headerBytes.data()), headerBytes.size()});
    auto payload = ParseJson({reinterpret_cast<char*>(payloadBytes.data()), payloadBytes.size()});

    if(!header.IsValid() || !payload.IsValid())
        return {CryptoInvalidArg, std::move(parts)};

    parts.header = std::move(header.object);
    parts.payload = std::move(payload.object);
    parts.signature = std::move(sig);

    // Safe to take after the moves above: JsonObject's move only transfers an internal handle,
    // the underlying string storage it points into never relocates
    parts.alg = parts.header.Get("alg").AsString();
    parts.kid = parts.header.Get("kid").AsString();

    return {CryptoOk, std::move(parts)};
}

// "exp"/"nbf" are Unix timestamps embedded in the token at issue time. This is what stops an
// old (but still correctly signed) token from working forever, and what stops a token from
// being used before its intended start time
inline bool JwtTimeClaimsValid(const JsonObject& payload) noexcept
{
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    auto exp = payload.Get("exp");
    if(!exp.IsInt() && !exp.IsUInt())
        return false;

    const auto expVal = exp.IsUInt() ? static_cast<std::int64_t>(exp.AsUInt()) : exp.AsInt();
    if(now >= expVal)
        return false;

    auto nbf = payload.Get("nbf");
    if(nbf.IsInt() || nbf.IsUInt()) {
        const auto nbfVal = nbf.IsUInt() ? static_cast<std::int64_t>(nbf.AsUInt()) : nbf.AsInt();
        if(now < nbfVal)
            return false;
    }

    return true;
}

// "aud" identifies who the token was issued for. Checking it stops a token that is otherwise
// validly signed by the same issuer, but meant for a different audience, from being accepted
// here. Per RFC 7519, "aud" may be a bare string or an array of strings, both are handled
inline bool JwtAudienceMatches(const JsonObject& payload, std::string_view expectedAud) noexcept
{
    auto aud = payload.Get("aud");

    if(aud.IsString())
        return aud.AsString() == expectedAud;

    if(aud.IsArray()) {
        for(std::uint32_t i = 0; i < aud.Length(); i++) {
            auto entry = aud[i];
            if(entry.IsString() && entry.AsString() == expectedAud)
                return true;
        }
    }

    return false;
}

namespace Detail {

// Every alg name recognized below is exactly 5 bytes ("RS256".."EdDSA"), packed big-endian into
// a uint64_t so the dispatch is one integer switch instead of a chain of string comparisons,
// letting the compiler turn it into a jump table/binary search instead of independent branches
inline constexpr std::uint64_t PackAlg5(std::string_view s) noexcept
{
    std::uint64_t key = 0;
    for(const char c : s)
        key = (key << 8) | static_cast<std::uint8_t>(c);

    return key;
}

inline bool JwtAlgToScheme(std::string_view alg, CryptoAsymScheme& out) noexcept
{
    if(alg.size() != 5)
        return false;

    // clang-format off
    switch(PackAlg5(alg)) {
        case PackAlg5("RS256"): out = CryptoRs256; return true;
        case PackAlg5("RS384"): out = CryptoRs384; return true;
        case PackAlg5("RS512"): out = CryptoRs512; return true;
        case PackAlg5("PS256"): out = CryptoPs256; return true;
        case PackAlg5("PS384"): out = CryptoPs384; return true;
        case PackAlg5("PS512"): out = CryptoPs512; return true;
        case PackAlg5("ES256"): out = CryptoEs256; return true;
        case PackAlg5("ES384"): out = CryptoEs384; return true;
        case PackAlg5("EdDSA"): out = CryptoEd25519; return true; // JOSE's name for Ed25519, RFC 8037
        default: return false;
    }
    // clang-format on
}

} // namespace Detail

// Verifies 'parts.signingInput' against 'parts.signature' using 'key', dispatching on the
// header's own "alg" rather than assuming one, a token whose alg wasn't recognized returns
// CryptoUnsupported without touching the key at all
inline CryptoStatus VerifyJwtSignature(const JwtParts& parts, const AsymKey& key)
{
    CryptoAsymScheme scheme{};
    if(!Detail::JwtAlgToScheme(parts.alg, scheme))
        return CryptoUnsupported;

    const auto sigView =
        std::string_view(reinterpret_cast<const char*>(parts.signature.data()), parts.signature.size());

    return key.Verify(scheme, parts.signingInput, sigView);
}

} // namespace WFX

#endif // WFX_INC_WFX_UTILS_JWT_HPP
