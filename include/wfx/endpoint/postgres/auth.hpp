// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_POSTGRES_AUTH_HPP
#define WFX_INC_WFX_ENDPOINT_POSTGRES_AUTH_HPP

// -----------------------------------------------------------------------
// Postgres authentication.
//
// SCRAM-SHA-256 (RFC 5802 / RFC 7677) is the mechanism Postgres has defaulted
// to since version 10, and the only one implemented here beyond cleartext.
// It runs as four messages after AuthenticationSASL:
//
//   client-first   n,,n=,r=<client-nonce>
//   server-first   r=<client-nonce><server-nonce>,s=<salt>,i=<iterations>
//   client-final   c=biws,r=<nonce>,p=<proof>
//   server-final   v=<server-signature>
// -----------------------------------------------------------------------

#include "protocol.hpp"
#include "types.hpp"
#include "wfx/memory.hpp"
#include "wfx/utils/crypto.hpp"
#include "wfx/utils/encoding.hpp"

#include <cstdint>
#include <string_view>

namespace WFX::Postgres::Detail {

// Which mechanisms an endpoint will agree to. Anything weaker than the policy
// allows fails the handshake instead of silently downgrading.
enum class PgAuthPolicy : std::uint8_t {
    ANY,          // whatever the server asks for, cleartext included
    NO_PLAINTEXT, // refuse cleartext, allow SCRAM
    SCRAM_ONLY,   // refuse everything except SCRAM
};

inline constexpr std::string_view SCRAM_SHA_256 = "SCRAM-SHA-256";

// "n,," base64 encoded, the gs2-header echoed back in client-final
inline constexpr std::string_view SCRAM_GS2_HEADER = "n,,";
inline constexpr std::string_view SCRAM_GS2_HEADER_B64 = "biws";

// 18 raw bytes, matching libpq, which is 24 characters once base64 encoded
inline constexpr std::uint32_t SCRAM_NONCE_BYTES = 18;

// A server asking for more than this is either broken or burning our CPU
inline constexpr std::uint32_t SCRAM_MAX_ITERATIONS = 1000000;

// Every key, proof and signature in the exchange is one SHA-256 digest wide
inline constexpr std::uint32_t SCRAM_DIGEST_LEN = WFX::DigestLenFor<WFX::CryptoHashAlgo::SHA256>();

// -----------------------------------------------------------------------
// Reads one comma separated attribute out of a SCRAM message. Values may
// themselves contain '=' (base64 padding does), so only the first one
// separates the key from its value.
// -----------------------------------------------------------------------
inline bool ScramAttribute(std::string_view msg, char key, std::string_view& out) noexcept
{
    std::size_t pos = 0;

    while(pos < msg.size()) {
        std::size_t end = msg.find(',', pos);
        if(end == std::string_view::npos)
            end = msg.size();

        const std::string_view part = msg.substr(pos, end - pos);
        if(part.size() >= 2 && part[0] == key && part[1] == '=') {
            out = part.substr(2);
            return true;
        }

        pos = end + 1;
    }

    return false;
}

// -----------------------------------------------------------------------
// Picks a mechanism from the null terminated list in AuthenticationSASL. A
// server offering only SCRAM-SHA-256-PLUS fails here, since channel binding
// needs the peer certificate and user space cannot reach it.
// -----------------------------------------------------------------------
inline bool ScramSelectMechanism(std::string_view mechanisms) noexcept
{
    std::size_t pos = 0;

    while(pos < mechanisms.size()) {
        const std::size_t end = mechanisms.find('\0', pos);
        const std::string_view name =
            mechanisms.substr(pos, (end == std::string_view::npos ? mechanisms.size() : end) - pos);

        if(name.empty())
            break;

        if(name == SCRAM_SHA_256)
            return true;

        if(end == std::string_view::npos)
            break;

        pos = end + 1;
    }

    return false;
}

// -----------------------------------------------------------------------
// ScramSha256
//
// Holds the state that has to survive between messages: the nonce and bare
// first message needed to rebuild AuthMessage, and the server signature to
// check once the exchange completes.
// -----------------------------------------------------------------------
class ScramSha256 {
public: // Step 1, client-first
    // Produces the SASL initial response, gs2-header included
    bool BuildClientFirst(WFX::String& out)
    {
        auto [status, raw] = WFX::RandomBytes(SCRAM_NONCE_BYTES);
        if(status != WFX::CryptoOk)
            return false;

        // Unpadded, so the nonce cannot contain '=' and confuse an attribute split
        const WFX::String nonce =
            WFX::Base64Encode({reinterpret_cast<const char*>(raw.data()), raw.size()}, false, false);

        // Postgres takes the user from the startup message and ignores the one
        // here, so it is left empty exactly as libpq leaves it
        return BuildClientFirst(nonce, {}, out);
    }

    // Nonce and user supplied rather than generated, which keeps the exchange
    // reproducible against a known vector
    bool BuildClientFirst(std::string_view nonce, std::string_view user, WFX::String& out)
    {
        if(nonce.empty())
            return false;

        clientNonce_.assign(nonce.data(), nonce.size());

        clientFirstBare_.assign("n=");
        clientFirstBare_.append(user.data(), user.size());
        clientFirstBare_.append(",r=");
        clientFirstBare_.append(clientNonce_);

        out.assign(SCRAM_GS2_HEADER);
        out.append(clientFirstBare_);
        return true;
    }

public: // Step 2, server-first to client-final
    bool BuildClientFinal(std::string_view serverFirst, std::string_view password, WFX::String& out)
    {
        std::string_view nonce;
        std::string_view saltB64;
        std::string_view iterStr;

        if(!ScramAttribute(serverFirst, 'r', nonce) || !ScramAttribute(serverFirst, 's', saltB64) ||
           !ScramAttribute(serverFirst, 'i', iterStr))
            return false;

        // The server nonce must extend ours, otherwise the exchange is not
        // bound to the nonce we generated
        if(nonce.size() <= clientNonce_.size() || nonce.compare(0, clientNonce_.size(), clientNonce_) != 0)
            return false;

        const std::uint32_t iterations = DecodeText<std::uint32_t>(iterStr);
        if(iterations == 0 || iterations > SCRAM_MAX_ITERATIONS)
            return false;

        auto [saltOk, salt] = WFX::Base64Decode(saltB64);
        if(!saltOk || salt.empty())
            return false;

        auto [pbkStatus, saltedPassword] =
            WFX::Pbkdf2(password, {reinterpret_cast<const char*>(salt.data()), salt.size()}, iterations,
                        SCRAM_DIGEST_LEN);
        if(pbkStatus != WFX::CryptoOk)
            return false;

        const std::string_view salted{reinterpret_cast<const char*>(saltedPassword.data()), saltedPassword.size()};

        auto [ckStatus, clientKey] = WFX::HmacSha256(salted, "Client Key");
        auto [skStatus, serverKey] = WFX::HmacSha256(salted, "Server Key");
        if(ckStatus != WFX::CryptoOk || skStatus != WFX::CryptoOk)
            return false;

        auto [shStatus, storedKey] = WFX::Sha256(clientKey.View());
        if(shStatus != WFX::CryptoOk)
            return false;

        // client-final-message-without-proof, which also closes out AuthMessage
        WFX::String withoutProof;
        withoutProof.assign("c=");
        withoutProof.append(SCRAM_GS2_HEADER_B64);
        withoutProof.append(",r=");
        withoutProof.append(nonce.data(), nonce.size());

        WFX::String authMessage;
        authMessage.assign(clientFirstBare_);
        authMessage.push_back(',');
        authMessage.append(serverFirst.data(), serverFirst.size());
        authMessage.push_back(',');
        authMessage.append(withoutProof);

        auto [csStatus, clientSignature] = WFX::HmacSha256(storedKey.View(), authMessage);
        auto [ssStatus, serverSignature] = WFX::HmacSha256(serverKey.View(), authMessage);
        if(csStatus != WFX::CryptoOk || ssStatus != WFX::CryptoOk)
            return false;

        // Proof is the client key masked by its signature, so the server can
        // recover the key it stores without ever seeing the password
        std::uint8_t proof[SCRAM_DIGEST_LEN];
        for(std::uint32_t i = 0; i < sizeof(proof); ++i)
            proof[i] = static_cast<std::uint8_t>(clientKey.bytes[i] ^ clientSignature.bytes[i]);

        expectedServerSig_ = serverSignature;
        haveExpectedSig_ = true;

        out.assign(withoutProof);
        out.append(",p=");
        out.append(WFX::Base64Encode({reinterpret_cast<const char*>(proof), sizeof(proof)}));
        return true;
    }

public: // Step 3, server-final
    bool VerifyServerFinal(std::string_view serverFinal) const
    {
        if(!haveExpectedSig_)
            return false;

        std::string_view sigB64;
        if(!ScramAttribute(serverFinal, 'v', sigB64))
            return false;

        auto [ok, sig] = WFX::Base64Decode(sigB64);
        if(!ok)
            return false;

        return WFX::ConstantTimeEquals({reinterpret_cast<const char*>(sig.data()), sig.size()},
                                       expectedServerSig_.View());
    }

    // Servers report a failed exchange as e=<reason> in place of v=
    static bool ServerFinalIsError(std::string_view serverFinal, std::string_view& reason) noexcept
    {
        return ScramAttribute(serverFinal, 'e', reason);
    }

private:
    WFX::String clientNonce_;
    WFX::String clientFirstBare_;
    WFX::Digest<SCRAM_DIGEST_LEN> expectedServerSig_{};
    bool haveExpectedSig_ = false;
};

} // namespace WFX::Postgres::Detail

#endif // WFX_INC_WFX_ENDPOINT_POSTGRES_AUTH_HPP
