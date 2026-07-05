// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// Endpoint audit target. Unlike the inbound torture app, every route here turns
// an *inbound* request into an *outbound* WFX::HttpEndpoint call against the
// scriptable mock upstream (upstream.py, pinned at 127.0.0.1:UPSTREAM_PORT) and
// reflects the result back as JSON the harness can assert on:
//
//   { "ep": <EndpointStatus int>,          // 0 == SUCCESS, see shared/abis/types.hpp
//     "status": <upstream HTTP status>,     // present only when ep == 0
//     "bodylen": <response body length>,    //   "
//     "body": "<response body>",            //   "  (mock bodies are small ASCII)
//     "hdr": "<value of X-Want header>" }   //   "  (only if X-Want was sent)
//
// The inbound request selects what to do via headers (kept header-only so the
// harness never has to encode anything into the path):
//
//   X-Ep      default | small | fast | coalesce   (which endpoint instance)
//   X-Method  GET | HEAD | OPTIONS | DELETE | POST | PUT | PATCH   (default GET)
//   X-Path    upstream request target                             (default "/ok")
//   X-Body    request body for POST/PUT/PATCH                      (optional)
//   X-Fwd     a header to forward upstream, "Name: Value"          (optional)
//   X-Want    upstream response header to echo back into "hdr"     (optional)
//
// The four endpoint instances differ only in the knobs each test needs:
//   default  — roomy limits, the everyday client
//   small    — tiny header/body caps so limit enforcement is cheap to trigger
//   fast     — 1s request timeout so slow-upstream cases fail fast
//   coalesce — coalesceKey set, to prove concurrent-request dedup

#include <wfx/http.hpp>
#include <wfx/endpoint/http.hpp>

#include <cstdint>
#include <string_view>
#include <utility>

// The mock upstream is pinned here at COMPILE time (HttpEndpoint bakes host:port
// into the instance). The harness MUST launch upstream.py on this exact port; it
// is also mirrored in endpoint_audit.py as UPSTREAM_PORT.
#define UPSTREAM "127.0.0.1:8091"

// ─────────────────────────────────────────────────────────────────────────────
// Endpoint instances
// ─────────────────────────────────────────────────────────────────────────────

// Everyday client: roomy limits, plaintext (the mock speaks cleartext HTTP/1.1).
inline const auto EpDefault = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 4,
    .requestTimeoutSeconds = 10,
    .tlsConfig             = WFX::EpTlsInsecure,
}};

// Deliberately tiny caps so the harness can trip maxHeaderBytes / maxHeaderCount /
// maxBodyBytes with small, fast upstream responses instead of multi-MB payloads.
inline const auto EpSmall = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 2,
    .requestTimeoutSeconds = 10,
    .tlsConfig             = WFX::EpTlsInsecure,
    .maxHeaderBytes        = 256,
    .maxBodyBytes          = 1024,
    .maxHeaderCount        = 8,
}};

// Minimum request budget the engine allows (timeouts must be >= the 5s timer-tick
// cooldown, INVOKE_TIMEOUT_COOLDOWN). slow-header / slow-body upstreams stall well
// past this, so they surface as EpRequestTimeout.
inline const auto EpFast = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 2,
    .connectTimeoutSeconds = 5,
    .requestTimeoutSeconds = 5,
    .tlsConfig             = WFX::EpTlsInsecure,
}};

// Coalesce identical concurrent GETs into one backend call. Key = FNV-1a(path);
// non-GET returns 0 (never coalesced), matching the header's documented pattern.
inline std::uint64_t CoalesceByPath(const void* reqVoid) noexcept
{
    const auto& r = *static_cast<const WFX::HttpEndpointRequest*>(reqVoid);
    if(r.method != WFX::HttpMethod::GET)
        return 0;

    std::uint64_t h = 1469598103934665603ull; // FNV-1a 64
    for(char c : r.path) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h ? h : 1; // 0 is reserved for "don't coalesce"
}

inline const auto EpCoalesce = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 4,
    .requestTimeoutSeconds = 10,
    .tlsConfig             = WFX::EpTlsInsecure,
    .coalesceKey           = &CoalesceByPath,
}};

// Single-slot pool: forces every sequential request onto the SAME pooled
// connection + slot state, so the harness can prove there is no cross-request
// bleed of body / headers / status across keep-alive reuse.
inline const auto EpReuse = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 1,
    .requestTimeoutSeconds = 10,
    .tlsConfig             = WFX::EpTlsInsecure,
}};

// ── Connection-lifecycle probes ─────────────────────────────────────────────

// Points at a port with nothing listening: connect() is refused (RST) -> the
// request must fail cleanly and quickly, never hang the worker.
inline const auto EpDead = WFX::HttpEndpoint{"127.0.0.1:9", WFX::HttpEndpointConfig{
    .connLimit                   = 1,
    .connectTimeoutSeconds       = 5,
    .requestTimeoutSeconds       = 5,
    .maxReconnectAttempts        = 1,
    .reconnectBackoffBaseSeconds = 1,
    .reconnectBackoffMaxSeconds  = 1,
    .tlsConfig                   = WFX::EpTlsInsecure,
}};

// Points at an unrouteable TEST-NET-1 address (RFC 5737): the SYN is black-holed,
// so the connect attempt must surface as an error within the connect/request budget.
inline const auto EpUnreach = WFX::HttpEndpoint{"192.0.2.1:80", WFX::HttpEndpointConfig{
    .connLimit                   = 1,
    .connectTimeoutSeconds       = 5,
    .requestTimeoutSeconds       = 5,
    .maxReconnectAttempts        = 1,
    .reconnectBackoffBaseSeconds = 1,
    .reconnectBackoffMaxSeconds  = 1,
    .tlsConfig                   = WFX::EpTlsInsecure,
}};

// Smallest idle timeout the engine allows (>= 5s tick cooldown) so a pooled
// keep-alive connection is recycled within the test window.
inline const auto EpIdle = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 1,
    .requestTimeoutSeconds = 5,
    .idleTimeoutSeconds    = 5,
    .tlsConfig             = WFX::EpTlsInsecure,
}};

// Eagerly opens 3 connections at startup (before any request is driven to it).
inline const auto EpPrewarm = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 4,
    .requestTimeoutSeconds = 5,
    .tlsConfig             = WFX::EpTlsInsecure,
    .prewarm               = 3,
}};

// ─────────────────────────────────────────────────────────────────────────────
// Result reflection
// ─────────────────────────────────────────────────────────────────────────────

template <typename OutT>
static void Emit(WFX::Request& req, WFX::Response& res, WFX::Shared::EndpointStatus st, OutT& out)
{
    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", static_cast<std::uint64_t>(static_cast<unsigned>(st)));

    if(st == WFX::EpOk) {
        j.Write("status", static_cast<std::uint64_t>(out->status));
        j.Write("bodylen", static_cast<std::uint64_t>(out->body.size()));
        j.Write("body", std::string_view{out->body.data(), out->body.size()});

        std::string_view want;
        if(req.GetHeader("X-Want", want)) {
            std::string_view hv;
            j.Write("hdr", out->GetHeader(want, hv) ? hv : std::string_view{});
        }
    }
}

// Map the X-Method header to the enum; unknown -> GET.
static WFX::HttpMethod MethodOf(std::string_view m) noexcept
{
    if(m == "HEAD")    return WFX::HttpMethod::HEAD;
    if(m == "OPTIONS") return WFX::HttpMethod::OPTIONS;
    if(m == "DELETE")  return WFX::HttpMethod::DELETE;
    if(m == "POST")    return WFX::HttpMethod::POST;
    if(m == "PUT")     return WFX::HttpMethod::PUT;
    if(m == "PATCH")   return WFX::HttpMethod::PATCH;
    return WFX::HttpMethod::GET;
}

static const WFX::HttpEndpoint* EndpointOf(std::string_view e) noexcept
{
    if(e == "small")    return &EpSmall;
    if(e == "fast")     return &EpFast;
    if(e == "coalesce") return &EpCoalesce;
    if(e == "reuse")    return &EpReuse;
    if(e == "dead")     return &EpDead;
    if(e == "unreach")  return &EpUnreach;
    if(e == "idle")     return &EpIdle;
    if(e == "prewarm")  return &EpPrewarm;
    return &EpDefault;
}

// ─────────────────────────────────────────────────────────────────────────────
// Routes
// ─────────────────────────────────────────────────────────────────────────────

WFX_GET("/health", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

// The one generic proxy route. All outbound behaviour is driven by X-* headers
// (see the file header). Returns the reflected JSON described above.
WFX_GET("/call", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view epName = "default", method = "GET", path = "/ok";
    req.GetHeader("X-Ep", epName);
    req.GetHeader("X-Method", method);
    req.GetHeader("X-Path", path);

    const WFX::HttpEndpoint* ep = EndpointOf(epName);
    WFX::HttpMethod m = MethodOf(method);

    // Up to three forwarded headers ("Name: Value") via X-Fwd / X-Fwd2 / X-Fwd3,
    // added in that order so the harness can assert header ordering and that a
    // forged Host/CL/TE is dropped even when surrounded by clean headers. The
    // views point into the inbound request buffer, which outlives this
    // coroutine's co_await — safe.
    WFX::HttpEndpointRequestHeaders hdrs;
    auto addFwd = [&](std::string_view hdrName) {
        std::string_view fwd;
        if(req.GetHeader(hdrName, fwd)) {
            auto colon = fwd.find(':');
            if(colon != std::string_view::npos) {
                std::string_view name = fwd.substr(0, colon);
                std::string_view val = fwd.substr(colon + 1);
                while(!val.empty() && val.front() == ' ')
                    val.remove_prefix(1);
                hdrs.Add(name, val);
            }
        }
    };
    addFwd("X-Fwd");
    addFwd("X-Fwd2");
    addFwd("X-Fwd3");

    std::string_view body;
    req.GetHeader("X-Body", body);

    // One co_await per method family; Emit reflects the outcome regardless of path.
    switch(m) {
        case WFX::HttpMethod::HEAD: {
            auto pr = co_await ep->Head(path, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        case WFX::HttpMethod::OPTIONS: {
            auto pr = co_await ep->Options(path, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        case WFX::HttpMethod::DELETE: {
            auto pr = co_await ep->Delete(path, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        case WFX::HttpMethod::POST: {
            auto pr = co_await ep->Post(path, body, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        case WFX::HttpMethod::PUT: {
            auto pr = co_await ep->Put(path, body, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        case WFX::HttpMethod::PATCH: {
            auto pr = co_await ep->Patch(path, body, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
        default: {
            auto pr = co_await ep->Get(path, std::move(hdrs));
            Emit(req, res, pr.first, pr.second);
            break;
        }
    }

    co_return;
})

// Serialize-side injection probe. The inbound POST *body* carries raw bytes that
// may contain CR/LF/NUL — bytes the inbound header parser would never allow, so
// this is the only way to feed the client serializer a genuinely hostile path or
// header. The client MUST refuse (EpSerializeError == 10), never emit a request
// that smuggles a second header/line upstream.
//
//   X-Ep      which endpoint (default)
//   X-Inject  "path"   -> request target is the raw body
//             "header" -> body is "Name:Value" (value may hold CR/LF/NUL), added as a header
//   body      the raw injection payload
WFX_POST("/inject", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view epName = "default", mode = "path";
    req.GetHeader("X-Ep", epName);
    req.GetHeader("X-Inject", mode);
    const WFX::HttpEndpoint* ep = EndpointOf(epName);

    std::string_view raw = req.Body(); // opaque bytes, may contain CR/LF/NUL

    WFX::HttpEndpointRequest r{};
    r.method = WFX::HttpMethod::GET;

    if(mode == "header") {
        r.path = "/ok";
        auto colon = raw.find(':');
        std::string_view name = colon == std::string_view::npos ? raw : raw.substr(0, colon);
        std::string_view val = colon == std::string_view::npos ? std::string_view{} : raw.substr(colon + 1);
        r.headers.Add(name, val);
    }
    else {
        r.path = raw; // hostile path straight into the request line
    }

    auto pr = co_await ep->Send(std::move(r));
    Emit(req, res, pr.first, pr.second);
    co_return;
})
