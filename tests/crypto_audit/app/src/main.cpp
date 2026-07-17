// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// Every route exists to give crypto_audit.py a surface to check wfx/utils/crypto.hpp
// against independently-computed (Python hashlib/hmac/AES-GCM) expected values.
// Binary inputs travel as hex-encoded headers (key/nonce/aad/salt/ikm/info/password);
// the primary data blob (hash/hmac input, AEAD plaintext/ciphertext) travels as the
// raw POST body.

#include <wfx/http.hpp>
#include <wfx/utils/crypto.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace {

std::string ToHex(std::string_view bytes)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for(unsigned char c : bytes) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

bool FromHex(std::string_view hex, std::string& out)
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

std::string HexHeader(WFX::Request& req, const char* name)
{
    std::string_view v;
    if(!req.GetHeader(name, v))
        return {};
    std::string decoded;
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
         phase = StreamPhase::DATA, footer = std::string{}, footerOffset = std::size_t{0},
         hasher = WFX::HashStream<Algo>()](WFX::Shared::StreamBuffer buf) mutable -> WFX::Shared::StreamResult {
            std::size_t written = 0;

            while(written < buf.size) {
                if(phase == StreamPhase::DATA) {
                    if(chunkIndex >= numChunks) {
                        auto [status, digest] = hasher.Final();
                        footer = "\n#DIGEST:";
                        footer += (status == WFX::CryptoOk) ? ToHex(digest.View()) : std::string("ERR");
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
    const std::string key = HexHeader(req, "X-Key");

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
    const std::string key = HexHeader(req, "X-Key");
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
    const std::string key = HexHeader(req, "X-Key");
    const std::string nonce = HexHeader(req, "X-Nonce");
    const std::string aad = HexHeader(req, "X-Aad");

    auto [s, out] = WFX::AeadEncrypt(algo, key, nonce, aad, req.Body());
    WriteStatusHex(res, s, out);
})

WFX_POST("/crypto/aead/decrypt", [](WFX::Request req, WFX::Response res) {
    const auto algo = AeadAlgoFromHeader(req);
    const std::string key = HexHeader(req, "X-Key");
    const std::string nonce = HexHeader(req, "X-Nonce");
    const std::string aad = HexHeader(req, "X-Aad");

    auto [s, out] = WFX::AeadDecrypt(algo, key, nonce, aad, req.Body());
    WriteStatusHex(res, s, out);
})

// vvv Key derivation vvv
WFX_GET("/crypto/pbkdf2", [](WFX::Request req, WFX::Response res) {
    const std::string password = HexHeader(req, "X-Password");
    const std::string salt = HexHeader(req, "X-Salt");
    const std::uint32_t iterations = UintHeader(req, "X-Iter", 1000);
    const std::uint32_t outLen = UintHeader(req, "X-Outlen", 32);

    auto [s, out] = WFX::Pbkdf2(password, salt, iterations, outLen);
    WriteStatusHex(res, s, out);
})

WFX_GET("/crypto/hkdf", [](WFX::Request req, WFX::Response res) {
    const std::string ikm = HexHeader(req, "X-Ikm");
    const std::string salt = HexHeader(req, "X-Salt");
    const std::string info = HexHeader(req, "X-Info");
    const std::uint32_t outLen = UintHeader(req, "X-Outlen", 32);

    auto [s, out] = WFX::Hkdf(ikm, salt, info, outLen);
    WriteStatusHex(res, s, out);
})

WFX_GET("/crypto/argon2id", [](WFX::Request req, WFX::Response res) {
    const std::string password = HexHeader(req, "X-Password");
    const std::string salt = HexHeader(req, "X-Salt");
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
    const std::string a = HexHeader(req, "X-A");
    const std::string b = HexHeader(req, "X-B");

    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("equal", WFX::ConstantTimeEquals(a, b));
})
