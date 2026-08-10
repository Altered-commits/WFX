// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// Every route exists to give crypto_audit.py a surface to check wfx/utils/crypto.hpp
// against independently-computed (Python hashlib/hmac/AES-GCM) expected values.
// Binary inputs travel as hex-encoded headers (key/nonce/aad/salt/ikm/info/password);
// the primary data blob (hash/hmac input, AEAD plaintext/ciphertext) travels as the
// raw POST body.

#include <wfx/http.hpp>
#include <wfx/memory.hpp>
#include <wfx/utils/crypto.hpp>
#include <wfx/utils/encoding.hpp>
#include <wfx/utils/jwk.hpp>
#include <wfx/utils/jwt.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace {

WFX::String ToHex(std::string_view bytes)
{
    static const char* digits = "0123456789abcdef";
    WFX::String out;
    out.reserve(bytes.size() * 2);
    for(unsigned char c : bytes) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

bool FromHex(std::string_view hex, WFX::String& out)
{
    if(hex.size() % 2 != 0)
        return false;

    out.clear();
    out.reserve(hex.size() / 2);

    auto nibble = [](char c) -> int {
        if(c >= '0' && c <= '9')
            return c - '0';
        if(c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if(c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };

    for(std::size_t i = 0; i < hex.size(); i += 2) {
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if(hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

WFX::String HexHeader(WFX::Request& req, const char* name)
{
    std::string_view v;
    if(!req.GetHeader(name, v))
        return {};
    WFX::String decoded;
    if(!FromHex(v, decoded))
        return {};
    return decoded;
}

std::uint32_t UintHeader(WFX::Request& req, const char* name, std::uint32_t def = 0)
{
    std::string_view v;
    if(!req.GetHeader(name, v) || v.empty())
        return def;
    std::uint32_t val = 0;
    for(char c : v) {
        if(c < '0' || c > '9')
            return def;
        val = val * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return val;
}

WFX::CryptoHashAlgo AlgoFromHeader(WFX::Request& req)
{
    std::string_view v;
    if(!req.GetHeader("X-Algo", v))
        return WFX::CryptoHashAlgo::SHA256;
    if(v == "sha384")
        return WFX::CryptoHashAlgo::SHA384;
    if(v == "sha512")
        return WFX::CryptoHashAlgo::SHA512;
    return WFX::CryptoHashAlgo::SHA256;
}

WFX::CryptoAeadAlgo AeadAlgoFromHeader(WFX::Request& req)
{
    std::string_view v;
    if(!req.GetHeader("X-Algo", v))
        return WFX::CryptoAesGcm;
    if(v == "chacha")
        return WFX::CryptoChaCha20Poly1305;
    return WFX::CryptoAesGcm;
}

WFX::CryptoAsymKeyType KeyTypeFromHeader(WFX::Request& req)
{
    std::string_view v;
    if(!req.GetHeader("X-Type", v))
        return WFX::CryptoRsaKey;
    if(v == "ec_p256")
        return WFX::CryptoEcP256Key;
    if(v == "ec_p384")
        return WFX::CryptoEcP384Key;
    if(v == "ed25519")
        return WFX::CryptoEd25519Key;
    return WFX::CryptoRsaKey;
}

WFX::CryptoAsymScheme SchemeFromHeader(WFX::Request& req)
{
    std::string_view v;
    if(!req.GetHeader("X-Scheme", v))
        return WFX::CryptoRs256;
    if(v == "rs384")
        return WFX::CryptoRs384;
    if(v == "rs512")
        return WFX::CryptoRs512;
    if(v == "ps256")
        return WFX::CryptoPs256;
    if(v == "ps384")
        return WFX::CryptoPs384;
    if(v == "ps512")
        return WFX::CryptoPs512;
    if(v == "es256")
        return WFX::CryptoEs256;
    if(v == "es384")
        return WFX::CryptoEs384;
    if(v == "ed25519")
        return WFX::CryptoEd25519;
    return WFX::CryptoRs256;
}

template <typename T> void WriteStatusHex(WFX::Response& res, WFX::CryptoStatus status, const T& data)
{
    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("status", static_cast<std::uint64_t>(status));
    w.Write("hex", ToHex(std::string_view(reinterpret_cast<const char*>(data.data()), data.size())));
}

// Streams deterministic content (numChunks * chunkLen bytes, chunk c filled with 'A'+c%26) out
// through res.Stream(), driving a WFX::HashStream<Algo> across however many callback invocations
// the engine chooses to split the buffer into. Once the last content byte has been produced, the
// digest becomes known and is appended in-band as a "\n#DIGEST:<hex>\n" footer - there's no
// trailer mechanism to hang it off after the fact, so the client just splits the response body on
// that marker to recover both the exact bytes streamed and the streaming hasher's verdict on them.
enum class StreamPhase : std::uint8_t { DATA, FOOTER, DONE };

template <WFX::CryptoHashAlgo Algo>
void RunHashStreamResponse(WFX::Response& res, std::uint32_t numChunks, std::uint32_t chunkLen)
{
    res.Status(200).Stream(
        [numChunks, chunkLen, chunkIndex = std::uint32_t{0}, withinChunk = std::uint32_t{0},
         phase = StreamPhase::DATA, footer = WFX::String{}, footerOffset = std::size_t{0},
         hasher = WFX::HashStream<Algo>()](WFX::Shared::StreamBuffer buf) mutable -> WFX::Shared::StreamResult {
            std::size_t written = 0;

            while(written < buf.size) {
                if(phase == StreamPhase::DATA) {
                    if(chunkIndex >= numChunks) {
                        auto [status, digest] = hasher.Final();
                        footer = "\n#DIGEST:";
                        footer += (status == WFX::CryptoOk) ? ToHex(digest.View()) : WFX::String("ERR");
                        footer += "\n";
                        footerOffset = 0;
                        phase = StreamPhase::FOOTER;
                        continue;
                    }

                    const char fill = static_cast<char>('A' + (chunkIndex % 26));
                    const std::uint32_t remainInChunk = chunkLen - withinChunk;
                    const std::size_t take = std::min<std::size_t>(remainInChunk, buf.size - written);

                    std::memset(buf.buffer + written, fill, take);
                    hasher.Update(std::string_view(buf.buffer + written, take));

                    withinChunk += static_cast<std::uint32_t>(take);
                    written += take;
                    if(withinChunk == chunkLen) {
                        ++chunkIndex;
                        withinChunk = 0;
                    }
                }
                else if(phase == StreamPhase::FOOTER) {
                    const std::size_t remain = footer.size() - footerOffset;
                    if(remain == 0) {
                        phase = StreamPhase::DONE;
                        break;
                    }
                    const std::size_t take = std::min(remain, buf.size - written);
                    std::memcpy(buf.buffer + written, footer.data() + footerOffset, take);
                    footerOffset += take;
                    written += take;
                }
                else
                    break;
            }

            const auto action = (phase == StreamPhase::DONE) ? WFX::Shared::StreamAction::STOP_AND_ALIVE_CONN
                                                              : WFX::Shared::StreamAction::CONTINUE;
            return {written, action};
        });
}

} // namespace

// Liveness probe: polled by crypto_audit.py's Server.wait_ready()/alive(), same as every
// other audit app.
WFX_GET("/health", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

// vvv Hashing vvv
WFX_POST("/crypto/hash", [](WFX::Request req, WFX::Response res) {
    const auto algo = AlgoFromHeader(req);
    WFX::CryptoStatus status;
    if(algo == WFX::CryptoHashAlgo::SHA256) {
        auto [s, d] = WFX::Sha256(req.Body());
        status = s;
        WriteStatusHex(res, status, d.bytes);
        return;
    }
    if(algo == WFX::CryptoHashAlgo::SHA384) {
        auto [s, d] = WFX::Sha384(req.Body());
        status = s;
        WriteStatusHex(res, status, d.bytes);
        return;
    }
    auto [s, d] = WFX::Sha512(req.Body());
    status = s;
    WriteStatusHex(res, status, d.bytes);
})

WFX_POST("/crypto/hash-stream", [](WFX::Request req, WFX::Response res) {
    const auto algo = AlgoFromHeader(req);
    std::string_view body = req.Body();
    std::size_t mid = body.size() / 2;

    auto run = [&](auto stream) {
        stream.Update(body.substr(0, mid));
        stream.Update(body.substr(mid));
        auto [s, d] = stream.Final();
        WriteStatusHex(res, s, d.bytes);
    };

    if(algo == WFX::CryptoHashAlgo::SHA256)
        run(WFX::HashStream<WFX::CryptoHashAlgo::SHA256>());
    else if(algo == WFX::CryptoHashAlgo::SHA384)
        run(WFX::HashStream<WFX::CryptoHashAlgo::SHA384>());
    else
        run(WFX::HashStream<WFX::CryptoHashAlgo::SHA512>());
})

// Response body itself is streamed via res.Stream() while a HashStream digests it chunk-by-chunk
// across callback invocations; see RunHashStreamResponse's comment for the footer format.
WFX_GET("/crypto/hash-stream-response", [](WFX::Request req, WFX::Response res) {
    const auto algo = AlgoFromHeader(req);
    const std::uint32_t numChunks = UintHeader(req, "X-Chunks", 20);
    const std::uint32_t chunkLen = UintHeader(req, "X-Chunklen", 64);

    if(algo == WFX::CryptoHashAlgo::SHA256)
        RunHashStreamResponse<WFX::CryptoHashAlgo::SHA256>(res, numChunks, chunkLen);
    else if(algo == WFX::CryptoHashAlgo::SHA384)
        RunHashStreamResponse<WFX::CryptoHashAlgo::SHA384>(res, numChunks, chunkLen);
    else
        RunHashStreamResponse<WFX::CryptoHashAlgo::SHA512>(res, numChunks, chunkLen);
})

// vvv HMAC vvv
WFX_POST("/crypto/hmac", [](WFX::Request req, WFX::Response res) {
    const auto algo = AlgoFromHeader(req);
    const WFX::String key = HexHeader(req, "X-Key");

    if(algo == WFX::CryptoHashAlgo::SHA256) {
        auto [s, d] = WFX::HmacSha256(key, req.Body());
        WriteStatusHex(res, s, d.bytes);
    }
    else if(algo == WFX::CryptoHashAlgo::SHA384) {
        auto [s, d] = WFX::HmacSha384(key, req.Body());
        WriteStatusHex(res, s, d.bytes);
    }
    else {
        auto [s, d] = WFX::HmacSha512(key, req.Body());
        WriteStatusHex(res, s, d.bytes);
    }
})

WFX_POST("/crypto/hmac-stream", [](WFX::Request req, WFX::Response res) {
    const auto algo = AlgoFromHeader(req);
    const WFX::String key = HexHeader(req, "X-Key");
    std::string_view body = req.Body();
    std::size_t mid = body.size() / 2;

    auto run = [&](auto stream) {
        stream.Update(body.substr(0, mid));
        stream.Update(body.substr(mid));
        auto [s, d] = stream.Final();
        WriteStatusHex(res, s, d.bytes);
    };

    if(algo == WFX::CryptoHashAlgo::SHA256)
        run(WFX::HmacStream<WFX::CryptoHashAlgo::SHA256>(key));
    else if(algo == WFX::CryptoHashAlgo::SHA384)
        run(WFX::HmacStream<WFX::CryptoHashAlgo::SHA384>(key));
    else
        run(WFX::HmacStream<WFX::CryptoHashAlgo::SHA512>(key));
})

// vvv AEAD vvv
WFX_POST("/crypto/aead/encrypt", [](WFX::Request req, WFX::Response res) {
    const auto algo = AeadAlgoFromHeader(req);
    const WFX::String key = HexHeader(req, "X-Key");
    const WFX::String nonce = HexHeader(req, "X-Nonce");
    const WFX::String aad = HexHeader(req, "X-Aad");

    auto [s, out] = WFX::AeadEncrypt(algo, key, nonce, aad, req.Body());
    WriteStatusHex(res, s, out);
})

WFX_POST("/crypto/aead/decrypt", [](WFX::Request req, WFX::Response res) {
    const auto algo = AeadAlgoFromHeader(req);
    const WFX::String key = HexHeader(req, "X-Key");
    const WFX::String nonce = HexHeader(req, "X-Nonce");
    const WFX::String aad = HexHeader(req, "X-Aad");

    auto [s, out] = WFX::AeadDecrypt(algo, key, nonce, aad, req.Body());
    WriteStatusHex(res, s, out);
})

// vvv Key derivation vvv
WFX_GET("/crypto/pbkdf2", [](WFX::Request req, WFX::Response res) {
    const WFX::String password = HexHeader(req, "X-Password");
    const WFX::String salt = HexHeader(req, "X-Salt");
    const std::uint32_t iterations = UintHeader(req, "X-Iter", 1000);
    const std::uint32_t outLen = UintHeader(req, "X-Outlen", 32);

    auto [s, out] = WFX::Pbkdf2(password, salt, iterations, outLen);
    WriteStatusHex(res, s, out);
})

WFX_GET("/crypto/hkdf", [](WFX::Request req, WFX::Response res) {
    const WFX::String ikm = HexHeader(req, "X-Ikm");
    const WFX::String salt = HexHeader(req, "X-Salt");
    const WFX::String info = HexHeader(req, "X-Info");
    const std::uint32_t outLen = UintHeader(req, "X-Outlen", 32);

    auto [s, out] = WFX::Hkdf(ikm, salt, info, outLen);
    WriteStatusHex(res, s, out);
})

WFX_GET("/crypto/argon2id", [](WFX::Request req, WFX::Response res) {
    const WFX::String password = HexHeader(req, "X-Password");
    const WFX::String salt = HexHeader(req, "X-Salt");
    const std::uint32_t iterations = UintHeader(req, "X-Iter", 2);
    const std::uint32_t memoryKb = UintHeader(req, "X-Memkb", 8192);
    const std::uint32_t parallelism = UintHeader(req, "X-Parallelism", 1);
    const std::uint32_t outLen = UintHeader(req, "X-Outlen", 32);

    auto [s, out] = WFX::Argon2id(password, salt, iterations, memoryKb, parallelism, outLen);
    WriteStatusHex(res, s, out);
})

// vvv Misc vvv
WFX_GET("/crypto/random", [](WFX::Request req, WFX::Response res) {
    const std::uint32_t len = UintHeader(req, "X-Len", 32);
    auto [s, out] = WFX::RandomBytes(len);
    WriteStatusHex(res, s, out);
})

WFX_GET("/crypto/consttime", [](WFX::Request req, WFX::Response res) {
    const WFX::String a = HexHeader(req, "X-A");
    const WFX::String b = HexHeader(req, "X-B");

    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("equal", WFX::ConstantTimeEquals(a, b));
})

// vvv Asymmetric vvv
// X-Key/X-Sig always travel hex-encoded as headers (even a 4096-bit RSA PEM fits well under
// max_header_size), the message being signed/verified is the raw POST body so it can be driven
// up to max_body_size for overflow/truncation checks independent of key size
WFX_POST("/crypto/asym/keygen", [](WFX::Request req, WFX::Response res) {
    const auto type = KeyTypeFromHeader(req);
    const std::uint32_t bits = UintHeader(req, "X-Bits", 2048);

    auto [s, key] = WFX::AsymKey::Generate(type, bits);

    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("status", static_cast<std::uint64_t>(s));
    if(s != WFX::CryptoOk) {
        w.Write("private_pem", WFX::String{});
        w.Write("public_pem", WFX::String{});
        return;
    }

    auto [ps, priv] = key.ExportPrivate();
    auto [qs, pub] = key.ExportPublic();
    w.Write("private_pem", ps == WFX::CryptoOk
                                ? ToHex(std::string_view(reinterpret_cast<const char*>(priv.data()), priv.size()))
                                : WFX::String{});
    w.Write("public_pem", qs == WFX::CryptoOk
                               ? ToHex(std::string_view(reinterpret_cast<const char*>(pub.data()), pub.size()))
                               : WFX::String{});
})

WFX_POST("/crypto/asym/sign", [](WFX::Request req, WFX::Response res) {
    const WFX::String keyPem = HexHeader(req, "X-Key");
    const auto scheme = SchemeFromHeader(req);

    auto [ls, key] = WFX::AsymKey::Load(keyPem, true);
    if(ls != WFX::CryptoOk) {
        WriteStatusHex(res, ls, WFX::Vector<std::uint8_t>{});
        return;
    }

    auto [s, sig] = key.Sign(scheme, req.Body());
    WriteStatusHex(res, s, sig);
})

WFX_POST("/crypto/asym/verify", [](WFX::Request req, WFX::Response res) {
    const WFX::String keyPem = HexHeader(req, "X-Key");
    const WFX::String sig = HexHeader(req, "X-Sig");
    const auto scheme = SchemeFromHeader(req);

    auto [ls, key] = WFX::AsymKey::Load(keyPem, false);

    res.Status(200);
    auto w = WFX::ImJson(res);
    if(ls != WFX::CryptoOk) {
        w.Write("status", static_cast<std::uint64_t>(ls));
        return;
    }

    const auto status = key.Verify(scheme, req.Body(), sig);
    w.Write("status", static_cast<std::uint64_t>(status));
})

// Loads X-Key (PEM, private or public per X-KeyIsPrivate) then re-exports per X-ExportPrivate -
// exercises AsymKeyPemLen/AsymKeyExport directly, including the public-only-handle mismatch case
WFX_POST("/crypto/asym/export", [](WFX::Request req, WFX::Response res) {
    const WFX::String keyPem = HexHeader(req, "X-Key");
    const bool keyIsPrivate = UintHeader(req, "X-KeyIsPrivate", 0) != 0;
    const bool exportPrivate = UintHeader(req, "X-ExportPrivate", 0) != 0;

    auto [ls, key] = WFX::AsymKey::Load(keyPem, keyIsPrivate);
    if(ls != WFX::CryptoOk) {
        WriteStatusHex(res, ls, WFX::Vector<std::uint8_t>{});
        return;
    }

    auto [s, out] = exportPrivate ? key.ExportPrivate() : key.ExportPublic();
    WriteStatusHex(res, s, out);
})

// N travels as the raw POST body (not a header) specifically so an oversized/malicious modulus
// can be driven up to max_body_size, X-E stays a header since a public exponent is always tiny
WFX_POST("/crypto/asym/from-rsa-public", [](WFX::Request req, WFX::Response res) {
    const WFX::String e = HexHeader(req, "X-E");

    auto [s, key] = WFX::AsymKey::FromRsaPublic(req.Body(), e);
    if(s != WFX::CryptoOk) {
        WriteStatusHex(res, s, WFX::Vector<std::uint8_t>{});
        return;
    }

    auto [es, pub] = key.ExportPublic();
    WriteStatusHex(res, es, pub);
})

WFX_POST("/crypto/asym/from-ec-public", [](WFX::Request req, WFX::Response res) {
    std::string_view curveStr;
    req.GetHeader("X-Curve", curveStr);
    const auto curve = curveStr == "p384" ? WFX::CryptoEcP384Key : WFX::CryptoEcP256Key;

    const WFX::String x = HexHeader(req, "X-X");
    const WFX::String y = HexHeader(req, "X-Y");

    auto [s, key] = WFX::AsymKey::FromEcPublic(curve, x, y);
    if(s != WFX::CryptoOk) {
        WriteStatusHex(res, s, WFX::Vector<std::uint8_t>{});
        return;
    }

    auto [es, pub] = key.ExportPublic();
    WriteStatusHex(res, es, pub);
})

// vvv JWK vvv
// JWKS JSON is the raw POST body (supports an oversized/hostile body up to max_body_size), kid
// stays a plain (non-hex) header since it's just a JSON string identifier the tests control
WFX_POST("/jwk/load", [](WFX::Request req, WFX::Response res) {
    std::string_view kid;
    req.GetHeader("X-Kid", kid);

    auto [s, key] = WFX::LoadJwk(req.Body(), kid);
    if(s != WFX::CryptoOk) {
        WriteStatusHex(res, s, WFX::Vector<std::uint8_t>{});
        return;
    }

    auto [es, pub] = key.ExportPublic();
    WriteStatusHex(res, es, pub);
})

// vvv JWT vvv
// Token travels as the raw POST body. X-Key (hex PEM public key) and X-Aud (expected
// audience) are optional: an absent X-Key stops after parsing/claims so malformed-token
// tests don't need a real key at all, an absent X-Aud skips the audience check entirely
WFX_POST("/jwt/verify", [](WFX::Request req, WFX::Response res) {
    auto [ps, parts] = WFX::ParseJwt(req.Body());

    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("parseStatus", static_cast<std::uint64_t>(ps));

    if(ps != WFX::CryptoOk) {
        w.Write("audOk", false);
        w.Write("timeOk", false);
        w.Write("verifyStatus", static_cast<std::uint64_t>(WFX::CryptoInvalidArg));
        return;
    }

    w.Write("alg", parts.alg);
    w.Write("kid", parts.kid);

    std::string_view expectedAud;
    const bool checkAud = req.GetHeader("X-Aud", expectedAud);
    w.Write("audOk", !checkAud || WFX::JwtAudienceMatches(parts.payload, expectedAud));
    w.Write("timeOk", WFX::JwtTimeClaimsValid(parts.payload));

    const WFX::String keyPem = HexHeader(req, "X-Key");
    if(keyPem.empty()) {
        w.Write("verifyStatus", static_cast<std::uint64_t>(WFX::CryptoInvalidArg));
        return;
    }

    auto [ks, key] = WFX::AsymKey::Load(keyPem, false);
    if(ks != WFX::CryptoOk) {
        w.Write("verifyStatus", static_cast<std::uint64_t>(ks));
        return;
    }

    w.Write("verifyStatus", static_cast<std::uint64_t>(WFX::VerifyJwtSignature(parts, key)));
})

// vvv Encoding vvv
WFX_POST("/encoding/base64/encode", [](WFX::Request req, WFX::Response res) {
    const bool urlSafe = UintHeader(req, "X-Urlsafe", 0) != 0;
    const bool padded = UintHeader(req, "X-Padded", 1) != 0;

    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("b64", WFX::Base64Encode(req.Body(), urlSafe, padded));
})

WFX_POST("/encoding/base64/decode", [](WFX::Request req, WFX::Response res) {
    auto [ok, out] = WFX::Base64Decode(req.Body());

    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("ok", ok);
    w.Write("hex", ok ? ToHex(std::string_view(reinterpret_cast<const char*>(out.data()), out.size()))
                       : WFX::String{});
})

WFX_POST("/encoding/hex/encode", [](WFX::Request req, WFX::Response res) {
    const bool upper = UintHeader(req, "X-Upper", 0) != 0;

    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("hex", WFX::HexEncode(req.Body(), upper));
})

WFX_POST("/encoding/hex/decode", [](WFX::Request req, WFX::Response res) {
    auto [ok, out] = WFX::HexDecode(req.Body());

    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("ok", ok);
    w.Write("hex", ok ? WFX::HexEncode(std::string_view(reinterpret_cast<const char*>(out.data()), out.size()))
                       : WFX::String{});
})

WFX_POST("/encoding/url/encode", [](WFX::Request req, WFX::Response res) {
    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("url", WFX::UrlEncode(req.Body()));
})

WFX_POST("/encoding/url/decode", [](WFX::Request req, WFX::Response res) {
    auto [ok, out] = WFX::UrlDecode(req.Body());

    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("ok", ok);
    w.Write("hex", ok ? ToHex(out) : WFX::String{});
})
