// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

// -----------------------------------------------------------------------
// Header check and lint translation unit for include/
//
// Nothing under engine/, http/, os_specific/, etc. ever includes include/wfx/*, those exist
// purely for user .so builds, and tests/*/app/ is outside scripts/tidy.sh's file scan. This TU is
// where the compiler and clang-tidy see that code, built the way a user .so sees it:
// WFX_ENGINE_BUILD is deliberately NOT defined for this target (see CMakeLists.txt).
//
// Including a header only exposes its declarations; a template is compiled and analyzed once
// something instantiates it. Each section below therefore uses one area of the header-only API
// the way an app would. The target is compiled (compile_check.yml) and linted (tidy.sh), never
// linked, so the routes registered at the end never run.
// -----------------------------------------------------------------------

#include "wfx/all.hpp"

#include <array>
#include <cstdint>
#include <string_view>

// vvv Compile-time API vvv
// Helpers documented as constexpr have to stay usable in constant expressions
static_assert(WFX::Fnv1a("wfx") != 0, "WFX::Fnv1a must stay constexpr");
static_assert(WFX::EndpointStatusName(WFX::EpRequestTimeout) == "REQUEST_TIMEOUT",
              "WFX::EndpointStatusName must stay constexpr");
static_assert(WFX::AeadTagLen(WFX::CryptoAesGcm) == 16, "WFX::AeadTagLen must stay constexpr");
static_assert(WFX::Base64EncodedLen(3) == 4 && WFX::HexEncodedLen(3) == 6,
              "the encoding length helpers must stay constexpr");

// vvv Coroutines vvv
// Task<T> carrying a value, and Task<> carrying none
static WFX::Task<int> CheckTaskValue()
{
    co_return 1;
}

static WFX::Task<> CheckTaskVoid()
{
    co_return;
}

// vvv JSON vvv
// Parsing a body, then answering through both serializers
static void CheckJson(WFX::Request req, WFX::Response res)
{
    const WFX::JsonParseResult parsed = WFX::ParseJson(req.Body());
    if(!parsed.IsValid()) {
        auto writer = WFX::ImJson(res);
        writer.Write("error", parsed.error);
        return;
    }

    auto reply = WFX::RmJson();
    reply["valid"] = true;
    reply["offset"] = static_cast<std::uint64_t>(parsed.offset);
    reply.Write(res);
}

// vvv Crypto vvv
// The out-buffer forms (the allocating forms are built on them) and the digest aliases
static bool CheckCrypto()
{
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, 12> nonce{};
    if(WFX::RandomBytes(key) != WFX::CryptoOk || WFX::RandomBytes(nonce) != WFX::CryptoOk)
        return false;

    const std::string_view keyView{reinterpret_cast<const char*>(key.data()), key.size()};
    const std::string_view nonceView{reinterpret_cast<const char*>(nonce.data()), nonce.size()};

    constexpr std::string_view PLAINTEXT = "header-check";

    std::array<std::uint8_t, PLAINTEXT.size() + WFX::AeadTagLen(WFX::CryptoAesGcm)> sealed{};
    const auto [sealStatus, sealedLen] = WFX::AeadEncrypt(WFX::CryptoAesGcm, keyView, nonceView, {}, PLAINTEXT, sealed);
    if(sealStatus != WFX::CryptoOk)
        return false;

    std::array<std::uint8_t, PLAINTEXT.size()> opened{};
    const std::string_view sealedView{reinterpret_cast<const char*>(sealed.data()), sealedLen};
    const auto [openStatus, openedLen] =
        WFX::AeadDecrypt(WFX::CryptoAesGcm, keyView, nonceView, {}, sealedView, opened);
    if(openStatus != WFX::CryptoOk || openedLen != PLAINTEXT.size())
        return false;

    std::array<std::uint8_t, 32> derived{};
    if(WFX::Argon2id("password", "salt-salt", 2, 19456, 1, derived) != WFX::CryptoOk)
        return false;

    const auto [hashStatus, digest] = WFX::Sha256(PLAINTEXT);
    const WFX::Sha256Digest& named = digest;

    return hashStatus == WFX::CryptoOk && WFX::ConstantTimeEquals(named.View(), digest.View());
}

// vvv Encoding vvv
// The out-buffer forms, sized by the length helpers
static bool CheckEncoding()
{
    constexpr std::string_view RAW = "wfx";

    std::array<char, WFX::Base64EncodedLen(RAW.size(), false)> base64{};
    const auto [encoded, base64Len] = WFX::Base64Encode(RAW, base64, true, false);

    std::array<std::uint8_t, WFX::Base64DecodedMaxLen(base64.size())> decoded{};
    const auto [decodedOk, decodedLen] = WFX::Base64Decode({base64.data(), base64Len}, decoded);

    std::array<char, WFX::HexEncodedLen(RAW.size())> hex{};
    const auto [hexOk, hexLen] = WFX::HexEncode(RAW, hex);

    return encoded && decodedOk && decodedLen == RAW.size() && hexOk && hexLen == hex.size();
}

// vvv Registration vvv
// The route and middleware macros instantiate their callback templates only from a real handler:
// one of each kind, sync and async
WFX_MIDDLEWARE("header-check-sync",
               [](WFX::Request, WFX::Response) -> WFX::MiddlewareAction { return WFX::MwContinue; })

WFX_MIDDLEWARE("header-check-async", [](WFX::Request, WFX::Response) -> WFX::MwCoro {
    co_await CheckTaskVoid();
    co_return WFX::MwContinue;
})

WFX_GET("/header-check/json", [](WFX::Request req, WFX::Response res) { CheckJson(req, res); })

WFX_GET("/header-check/utils", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    const int value = co_await CheckTaskValue();
    WFX::LogInfo("[HeaderCheck]: task returned ", value);

    res.SendText(CheckCrypto() && CheckEncoding() ? "ok" : "failed");
    co_return;
})
