// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// The "real" HTTP upstream for interop_audit. Not a mock: this is an ordinary WFX server,
// running real HTTPS (see config/wfx.local.toml's [SSL] section), that interop_audit/app's
// WFX::HttpEndpoint dials the same way it would dial any real HTTPS backend. Its own server
// side (routing, TLS, chunked streaming) is exactly the code every other WFX deployment runs,
// so driving a real client against it here is a genuine interop proof, not a stand-in.
//
// Routes:
//   GET    /health             liveness, polled by the harness before driving anything
//   GET    /get               fixed body + a custom response header, for header round-trip
//   POST   /echo               PUT   /echo         PATCH  /echo         DELETE /echo
//                              reflect method + body back, proving every verb round-trips
//   GET    /status/<code:uint> echoes the requested status code
//   GET    /delay/<ms:uint>    sleeps ms milliseconds before responding, for timeout behaviour
//   GET    /stream/<n:uint>    n real chunked lines via FlushStart/Write/Flush/FlushEnd
//   GET    /basic-auth         checks the Authorization header against a fixed credential

#include <wfx/http.hpp>
#include <wfx/async.hpp>
#include <wfx/memory.hpp>
#include <wfx/utils/encoding.hpp>

#include <cstdint>
#include <string_view>

WFX_GET("/health", [](WFX::Request req, WFX::Response res) {
    res.Status(200).SendText("ok");
})

WFX_GET("/get", [](WFX::Request req, WFX::Response res) {
    res.Header("X-Upstream", "wfx");
    res.Status(200).SendText("real upstream");
})

WFX_POST("/echo", [](WFX::Request req, WFX::Response res) {
    res.Header("X-Method", "POST");
    res.Status(200).SendText(req.Body());
})
WFX_PUT("/echo", [](WFX::Request req, WFX::Response res) {
    res.Header("X-Method", "PUT");
    res.Status(200).SendText(req.Body());
})
WFX_PATCH("/echo", [](WFX::Request req, WFX::Response res) {
    res.Header("X-Method", "PATCH");
    res.Status(200).SendText(req.Body());
})
WFX_DELETE("/echo", [](WFX::Request req, WFX::Response res) {
    res.Header("X-Method", "DELETE");
    res.Status(200).SendText(req.Body());
})

WFX_GET("/status/<code:uint>", [](WFX::Request req, WFX::Response res) {
    const auto code = static_cast<std::uint16_t>(req.GetSegment(0).AsU64());
    res.Status(code).SendText("s");
})

WFX_GET("/delay/<ms:uint>", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    const auto ms = static_cast<std::uint32_t>(req.GetSegment(0).AsU64());
    co_await WFX::SleepFor(ms);
    res.Status(200).SendText("done");
    co_return;
})

// A real chunked response, one line every 5ms so the client genuinely receives it in
// pieces rather than the whole thing landing in a single recv().
WFX_GET("/stream/<n:uint>", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    const auto n = req.GetSegment(0).AsU64();

    res.Header("Content-Type", "text/plain");
    res.FlushStart();

    for(std::uint64_t i = 0; i < n; i++) {
        res.Write("line ").Write(i).Write("\n");
        co_await WFX::SleepFor(5);
        co_await res.Flush();
    }

    co_await res.FlushEnd();
    co_return;
})

WFX_GET("/basic-auth", [](WFX::Request req, WFX::Response res) {
    std::string_view auth;
    WFX::String expected = "Basic ";
    expected += WFX::Base64Encode("interop:upstream-pass");

    if(!req.GetHeader("Authorization", auth) || auth != expected) {
        res.Status(401).SendText("nope");
        return;
    }
    res.Status(200).SendText("ok");
})
