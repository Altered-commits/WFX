// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// Audit target for the shipped protocol clients, WFX::HttpEndpoint and
// WFX::SmtpEndpoint.
//
// Every route turns an inbound request into an outbound call on one of the two
// clients, against a hostile mock, and reflects the outcome back as JSON the
// harness asserts on. Both mocks are byte oracles rather than conformant servers:
// http_upstream.py replays exactly the bytes it was handed, smtp_upstream.py runs
// the real handshake with one thing flipped per persona.
//
// Routes:
//   GET  /health        liveness
//   GET  /call          one HttpEndpoint call, everything chosen by X-* headers
//   POST /inject        feed the HTTP serializer a hostile path or header
//   POST /smtp/send     a full SMTP transaction, one command at a time
//   POST /smtp/send-mail  the same transaction through SmtpEndpoint::SendMail
//   POST /smtp/inject   feed the SMTP serializer a hostile field value
//   GET  /metrics       per-endpoint metrics table
//
// /call reflects:
//   { "ep": <EndpointStatus int>,         // 0 == SUCCESS, see shared/abis/types.hpp
//     "status": <upstream HTTP status>,   // the rest only when ep == 0
//     "bodylen": <response body length>,
//     "body": "<response body>",          // mock bodies are small ASCII
//     "hdr": "<value of the X-Want header>" }   // only if X-Want was sent
//
// and is driven header-only, so the harness never has to encode anything into a
// path it also wants to control byte for byte:
//
//   X-Ep      which HttpEndpoint instance, see EndpointOf   (default "default")
//   X-Method  GET | HEAD | OPTIONS | DELETE | POST | PUT | PATCH   (default GET)
//   X-Path    upstream request target                             (default "/ok")
//   X-Body    request body for POST/PUT/PATCH                      (optional)
//   X-Fwd / X-Fwd2 / X-Fwd3   headers to forward upstream, "Name: Value"
//   X-Want    upstream response header to echo back into "hdr"     (optional)
//
// The raw WFX::Endpoint<> primitive underneath both clients is audited separately,
// in tests/endpoint_audit.

#include <wfx/http.hpp>
#include <wfx/memory.hpp>
#include <wfx/telemetry.hpp>
#include <wfx/utils/hash.hpp>
#include <wfx/endpoint/http.hpp>
#include <wfx/endpoint/smtp.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// An endpoint bakes host:port into the instance, so the HTTP mock is pinned at
// COMPILE time. The harness must launch http_upstream.py on this exact port; it is
// mirrored in client_audit.py as HTTP_PORT.
#define UPSTREAM "127.0.0.1:8091"

// WFX::HttpEndpoint instances
//
// They differ only in the knobs one group of vectors needs: caps small enough to
// trip on a few hundred bytes, budgets short enough to time out inside the run, and
// hosts that cannot be reached at all.

// The everyday client: roomy limits, plaintext, since the mock speaks cleartext
// HTTP/1.1.
inline const auto EpDefault = WFX::HttpEndpoint{UPSTREAM, WFX::HttpEndpointConfig{
    .connLimit             = 4,
    .requestTimeoutSeconds = 10,
    .tlsConfig             = WFX::EpTlsInsecure,
}};

// Tiny header, body and header-count caps, so limit enforcement is reachable with
// a few hundred bytes.
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

// Coalesces identical concurrent GETs into one backend call. The key is FNV-1a over
// the path, and a non-GET returns 0, which is how http.hpp spells "never coalesce
// this one".
inline std::uint64_t CoalesceByPath(const void* reqVoid) noexcept
{
    const auto& r = *static_cast<const WFX::HttpEndpointRequest*>(reqVoid);
    if(r.method != WFX::HttpMethod::GET)
        return 0;

    const std::uint64_t h = WFX::Fnv1a(r.path);

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

// Points at a port with nothing listening, so connect() is refused with an RST and
// the request has to fail cleanly and quickly rather than hang the worker.
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

// WFX::SmtpEndpoint instances
//
// One per smtp_upstream.py persona. Both the port and the name are compiled in here
// and mirrored in client_audit.py's SMTP_PERSONAS table, keep the two in sync. The
// credentials match smtp_upstream.py's own defaults.
//
// Two budgets: SmtpCfg() gives 8s to personas that answer or refuse straight away,
// and SmtpCfgFast() drops that to 5s, the engine's floor, for the ones that never
// answer at all and can only be ended by the client's own timeout.
static WFX::SmtpEndpointConfig SmtpCfg(std::uint16_t connectTimeout = 8, std::uint16_t requestTimeout = 8) noexcept
{
    return WFX::SmtpEndpointConfig{
        .connLimit             = 4,
        .connectTimeoutSeconds = connectTimeout,
        .requestTimeoutSeconds = requestTimeout,
        .maxReconnectAttempts  = 0, // every call here is client-waited, background retry never applies
        .username              = "audituser",
        .password              = "audit-pass-123",
        .heloName              = "client-audit.wfx.test",
    };
}
static WFX::SmtpEndpointConfig SmtpCfgFast() noexcept
{
    // 5, not lower: EndpointConfig.connectTimeoutSeconds/requestTimeoutSeconds must be >=
    // INVOKE_TIMEOUT_COOLDOWN (5, the timeout timer's own tick period) or the engine refuses
    // to boot (see epoll_connection.cpp's BuildEndpoint-time Fatal check)
    return SmtpCfg(5, 5);
}

inline const auto Smtp_good               = WFX::SmtpEndpoint{"127.0.0.1:8100", SmtpCfg()};
// Dedicated pool for /smtp/inject, same mock persona/port as Smtp_good but never touched by
// /smtp/send. /smtp/send's "good" transactions never call Quit(), so a successful one leaves its
// connection pooled and still alive (ReturnEndpointToPool); Begin() has no way to tell a pooled
// alive slot from a genuinely fresh one, so phase_smtp_inject could silently inherit a connection
// left over from phase_smtp_handshake's earlier /smtp/send "good" calls instead of a clean one
inline const auto Smtp_good_injectroute   = WFX::SmtpEndpoint{"127.0.0.1:8100", SmtpCfg()};
inline const auto Smtp_auth_login_only    = WFX::SmtpEndpoint{"127.0.0.1:8101", SmtpCfg()};
inline const auto Smtp_inject             = WFX::SmtpEndpoint{"127.0.0.1:8103", SmtpCfg()};
inline const auto Smtp_no_starttls        = WFX::SmtpEndpoint{"127.0.0.1:8102", SmtpCfg()};
inline const auto Smtp_selfsigned         = WFX::SmtpEndpoint{"127.0.0.1:8104", SmtpCfg()};
inline const auto Smtp_wronghost          = WFX::SmtpEndpoint{"127.0.0.1:8105", SmtpCfg()};
inline const auto Smtp_expired            = WFX::SmtpEndpoint{"127.0.0.1:8106", SmtpCfg()};
inline const auto Smtp_auth_fail          = WFX::SmtpEndpoint{"127.0.0.1:8107", SmtpCfg()};
inline const auto Smtp_no_auth_mechs      = WFX::SmtpEndpoint{"127.0.0.1:8108", SmtpCfg()};
inline const auto Smtp_mismatched_code    = WFX::SmtpEndpoint{"127.0.0.1:8109", SmtpCfg()};
inline const auto Smtp_malformed_greeting = WFX::SmtpEndpoint{"127.0.0.1:8116", SmtpCfg()};
inline const auto Smtp_drop_greeting      = WFX::SmtpEndpoint{"127.0.0.1:8117", SmtpCfg()};
inline const auto Smtp_drop_pre_handshake = WFX::SmtpEndpoint{"127.0.0.1:8118", SmtpCfg()};
inline const auto Smtp_drop_starttls      = WFX::SmtpEndpoint{"127.0.0.1:8119", SmtpCfg()};
inline const auto Smtp_drop_auth          = WFX::SmtpEndpoint{"127.0.0.1:8120", SmtpCfg()};
inline const auto Smtp_drop_data_prompt   = WFX::SmtpEndpoint{"127.0.0.1:8121", SmtpCfg()};

inline const auto Smtp_flood_greeting     = WFX::SmtpEndpoint{"127.0.0.1:8110", SmtpCfgFast()};
inline const auto Smtp_flood_ehlo2        = WFX::SmtpEndpoint{"127.0.0.1:8111", SmtpCfgFast()};
inline const auto Smtp_huge_line_greeting = WFX::SmtpEndpoint{"127.0.0.1:8112", SmtpCfgFast()};
inline const auto Smtp_huge_line_ehlo2    = WFX::SmtpEndpoint{"127.0.0.1:8113", SmtpCfgFast()};
inline const auto Smtp_slow_trickle       = WFX::SmtpEndpoint{"127.0.0.1:8114", SmtpCfgFast()};
inline const auto Smtp_silent_data        = WFX::SmtpEndpoint{"127.0.0.1:8115", SmtpCfgFast()};

// heloName itself carries a CRLF-injection attempt. Points at the 'good' mock port, but the
// connection is never actually opened, SmtpOnConnect's HasInjectionBytes(opts->heloName) check
// fails before the first byte is sent (see smtp.hpp). Not part of SMTP_PERSONAS: no mock
// listener needs to exist for this one, the wire is never touched
inline const auto Smtp_heloinject = WFX::SmtpEndpoint{"127.0.0.1:8100", WFX::SmtpEndpointConfig{
    .connLimit             = 1,
    .connectTimeoutSeconds = 5,
    .requestTimeoutSeconds = 5,
    .maxReconnectAttempts  = 0,
    .username              = "audituser",
    .password              = "audit-pass-123",
    .heloName              = "evil\r\nMAIL FROM:<hacked@evil>",
}};

static const WFX::SmtpEndpoint* SmtpEndpointOf(std::string_view e) noexcept
{
    if(e == "auth_login_only")    return &Smtp_auth_login_only;
    if(e == "inject")             return &Smtp_inject;
    if(e == "no_starttls")        return &Smtp_no_starttls;
    if(e == "selfsigned")         return &Smtp_selfsigned;
    if(e == "wronghost")          return &Smtp_wronghost;
    if(e == "expired")            return &Smtp_expired;
    if(e == "auth_fail")          return &Smtp_auth_fail;
    if(e == "no_auth_mechs")      return &Smtp_no_auth_mechs;
    if(e == "mismatched_code")    return &Smtp_mismatched_code;
    if(e == "malformed_greeting") return &Smtp_malformed_greeting;
    if(e == "drop_greeting")      return &Smtp_drop_greeting;
    if(e == "drop_pre_handshake") return &Smtp_drop_pre_handshake;
    if(e == "drop_starttls")      return &Smtp_drop_starttls;
    if(e == "drop_auth")          return &Smtp_drop_auth;
    if(e == "drop_data_prompt")   return &Smtp_drop_data_prompt;
    if(e == "flood_greeting")     return &Smtp_flood_greeting;
    if(e == "flood_ehlo2")        return &Smtp_flood_ehlo2;
    if(e == "huge_line_greeting") return &Smtp_huge_line_greeting;
    if(e == "huge_line_ehlo2")    return &Smtp_huge_line_ehlo2;
    if(e == "slow_trickle")       return &Smtp_slow_trickle;
    if(e == "silent_data")        return &Smtp_silent_data;
    if(e == "heloinject")         return &Smtp_heloinject;
    return &Smtp_good;
}

// Reflecting the outcome

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

// EndpointStatus -> the plain integer JsonWriter has an overload for
static std::uint64_t EpJ(WFX::Shared::EndpointStatus s) noexcept
{
    return static_cast<std::uint64_t>(static_cast<unsigned>(s));
}

// Small unsigned header values (counts, sizes). Header-driven like every other
// knob in this app, so parsing lives in one place instead of per route.
static std::uint32_t HeaderU32(WFX::Request& req, const char* name, std::uint32_t fallback) noexcept
{
    std::string_view sv;
    if(!req.GetHeader(name, sv) || sv.empty())
        return fallback;

    std::uint32_t v = 0;
    for(char c : sv) {
        if(c < '0' || c > '9')
            return fallback;
        v = v * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return v;
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

// Routes
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
    // coroutine's co_await, so it is safe
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
// may contain CR/LF/NUL, bytes the inbound header parser would never allow, so
// this is the only way to feed the client serializer a genuinely hostile path or
// header. The client must refuse it (EpSerializeError), never emit a request that
// smuggles a second header or line upstream.
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

// WFX::SmtpEndpoint, driven end to end: MAIL FROM -> RCPT TO -> DATA -> body, all on one
// Reserve()'d connection. Stops at the first stage that fails, transport or protocol level.
//
//   X-Persona  which SmtpEndpointOf() persona (default "good")
//   X-From / X-FromName / X-To / X-ToName / X-Subject / X-ReplyTo   (all optional, sane defaults)
//   body       the message body (POST body, not a header, so it can carry raw CR/LF for
//              dot-stuffing round-trip tests)
//
//   { "ep": <EndpointStatus int>, "stage": "reserve"|"mail"|"rcpt"|"data_start"|"data_body"|"done",
//     "code": <SMTP reply code>, "success": <bool> }        // code/success only when ep == 0
WFX_POST("/smtp/send", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view personaName = "good";
    req.GetHeader("X-Persona", personaName);

    std::string_view fromAddr = "sender@wfx.test", fromName{}, toAddr = "recipient@wfx.test",
                     toName{}, subject = "audit", replyTo{};
    req.GetHeader("X-From", fromAddr);
    req.GetHeader("X-FromName", fromName);
    req.GetHeader("X-To", toAddr);
    req.GetHeader("X-ToName", toName);
    req.GetHeader("X-Subject", subject);
    req.GetHeader("X-ReplyTo", replyTo);
    std::string_view body = req.Body();

    const WFX::SmtpEndpoint* ep = SmtpEndpointOf(personaName);
    auto tx = ep->Begin();

    res.Status(200);
    auto j = WFX::ImJson(res);

    if(!tx.IsValid()) {
        j.Write("ep", EpJ(WFX::EpPoolExhausted));
        j.Write("stage", std::string_view{"reserve"});
        co_return;
    }

    auto [s1, r1] = co_await tx.MailFrom(fromAddr);
    if(s1 != WFX::EpOk) {
        j.Write("ep", EpJ(s1));
        j.Write("stage", std::string_view{"mail"});
        co_return;
    }
    j.Write("code", r1->code);
    j.Write("success", r1->Success());
    if(!r1->Success()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"mail"});
        co_return;
    }

    auto [s2, r2] = co_await tx.RcptTo(toAddr);
    if(s2 != WFX::EpOk) {
        j.Write("ep", EpJ(s2));
        j.Write("stage", std::string_view{"rcpt"});
        co_return;
    }
    j.Write("code", r2->code);
    j.Write("success", r2->Success());
    if(!r2->Success()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"rcpt"});
        co_return;
    }

    auto [s3, r3] = co_await tx.DataStart();
    if(s3 != WFX::EpOk) {
        j.Write("ep", EpJ(s3));
        j.Write("stage", std::string_view{"data_start"});
        co_return;
    }
    j.Write("code", r3->code);
    j.Write("success", r3->Continue());
    if(!r3->Continue()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"data_start"});
        co_return;
    }

    auto [s4, r4] = co_await tx.DataBody(fromAddr, fromName, toAddr, toName, subject, body, replyTo);
    if(s4 != WFX::EpOk) {
        j.Write("ep", EpJ(s4));
        j.Write("stage", std::string_view{"data_body"});
        co_return;
    }
    j.Write("ep", EpJ(WFX::EpOk));
    j.Write("code", r4->code);
    j.Write("success", r4->Success());
    j.Write("stage", std::string_view{r4->Success() ? "done" : "data_body"});
    co_return;
})

// Same personas and headers as /smtp/send, but drives the whole exchange through
// SmtpEndpoint::SendMail instead of calling MailFrom/RcptTo/DataStart/DataBody by hand.
// SendMail returns a WFX::Coro that this handler co_awaits, so it is what proves one
// coroutine can be awaited from another end to end rather than merely compile.
//
//   { "ep": <EndpointStatus int>, "code": <SMTP reply code, 0 if none arrived>, "success": <bool> }
WFX_POST("/smtp/send-mail", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view personaName = "good";
    req.GetHeader("X-Persona", personaName);

    std::string_view fromAddr = "sender@wfx.test", fromName{}, toAddr = "recipient@wfx.test",
                     toName{}, subject = "audit", replyTo{};
    req.GetHeader("X-From", fromAddr);
    req.GetHeader("X-FromName", fromName);
    req.GetHeader("X-To", toAddr);
    req.GetHeader("X-ToName", toName);
    req.GetHeader("X-Subject", subject);
    req.GetHeader("X-ReplyTo", replyTo);
    std::string_view body = req.Body();

    const WFX::SmtpEndpoint* ep = SmtpEndpointOf(personaName);

    WFX::SmtpSendOutcome outcome;
    co_await ep->SendMail(fromAddr, fromName, toAddr, toName, subject, body, outcome, replyTo);

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", EpJ(outcome.status));
    j.Write("code", outcome.response ? outcome.response->code : 0);
    j.Write("success", outcome.Success());
    co_return;
})

// Serialize-side injection probe for the SMTP client, mirrors HTTP's /inject: the injection
// screen (Smtp::Detail::HasInjectionBytes) only rejects CR/LF/NUL bytes that an HTTP *header*
// could never carry in the first place, so the hostile payload travels as the POST body and
// this route splices it into whichever field X-Field names. Always against the 'good' persona;
// fields ahead of the one under test get clean placeholder values so the run reaches it.
//
//   X-Field  mailfrom | rcptto | fromname | toname | subject | replyto | body   (default mailfrom)
//   body     the hostile payload (may contain CR/LF/NUL)
//
//   { "ep": <EndpointStatus int>, "stage": "mail"|"rcpt"|"data_start"|"data_body",
//     "code": <SMTP reply code>, "success": <bool> }   // code/success only on a clean-step stage;
//                                                       // EpSerializeError (11) is the correct
//                                                       // refusal on the field-under-test's stage
WFX_POST("/smtp/inject", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view field = "mailfrom";
    req.GetHeader("X-Field", field);
    std::string_view raw = req.Body();

    auto tx = Smtp_good_injectroute.Begin();

    res.Status(200);
    auto j = WFX::ImJson(res);
    if(!tx.IsValid()) {
        j.Write("ep", EpJ(WFX::EpPoolExhausted));
        j.Write("stage", std::string_view{"reserve"});
        co_return;
    }

    std::string_view mailFrom = (field == "mailfrom") ? raw : std::string_view{"sender@wfx.test"};
    auto [s1, r1] = co_await tx.MailFrom(mailFrom);
    if(field == "mailfrom") {
        j.Write("ep", EpJ(s1));
        j.Write("stage", std::string_view{"mail"});
        co_return;
    }
    if(s1 != WFX::EpOk) {
        j.Write("ep", EpJ(s1));
        j.Write("stage", std::string_view{"mail"});
        co_return;
    }
    if(!r1->Success()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"mail"});
        j.Write("code", r1->code);
        j.Write("success", false);
        co_return;
    }

    std::string_view rcptTo = (field == "rcptto") ? raw : std::string_view{"recipient@wfx.test"};
    auto [s2, r2] = co_await tx.RcptTo(rcptTo);
    if(field == "rcptto") {
        j.Write("ep", EpJ(s2));
        j.Write("stage", std::string_view{"rcpt"});
        co_return;
    }
    if(s2 != WFX::EpOk) {
        j.Write("ep", EpJ(s2));
        j.Write("stage", std::string_view{"rcpt"});
        co_return;
    }
    if(!r2->Success()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"rcpt"});
        j.Write("code", r2->code);
        j.Write("success", false);
        co_return;
    }

    auto [s3, r3] = co_await tx.DataStart();
    if(s3 != WFX::EpOk) {
        j.Write("ep", EpJ(s3));
        j.Write("stage", std::string_view{"data_start"});
        co_return;
    }
    if(!r3->Continue()) {
        j.Write("ep", EpJ(WFX::EpOk));
        j.Write("stage", std::string_view{"data_start"});
        j.Write("code", r3->code);
        j.Write("success", false);
        co_return;
    }

    std::string_view fromName = (field == "fromname") ? raw : std::string_view{};
    std::string_view toName   = (field == "toname")   ? raw : std::string_view{};
    std::string_view subject  = (field == "subject")  ? raw : std::string_view{"audit"};
    std::string_view replyTo  = (field == "replyto")  ? raw : std::string_view{};
    std::string_view body     = (field == "body")     ? raw : std::string_view{"hello"};

    auto [s4, r4] = co_await tx.DataBody("sender@wfx.test", fromName, "recipient@wfx.test", toName,
                                         subject, body, replyTo);
    j.Write("ep", EpJ(s4));
    j.Write("stage", std::string_view{"data_body"});
    if(s4 == WFX::EpOk) {
        j.Write("code", r4->code);
        j.Write("success", r4->Success());
    }
    co_return;
})

WFX_GET("/metrics", [](WFX::Request, WFX::Response res) {
    const bool latencyOn = WFX::MetricsLatencyEnabled();

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("latency_enabled", latencyOn);

    j.Arr("endpoints");
    for(std::uint16_t e = 0; e < WFX::EndpointMetricCount(); e++) {
        const auto ev = WFX::GetEndpointMetricsAt(e);
        j.Obj();
        j.Write("host", ev.host);
        j.Write("requests", ev.metrics.requests);
        j.Write("completed", ev.metrics.completed);
        j.Write("status_1xx", ev.metrics.status1xx);
        j.Write("status_2xx", ev.metrics.status2xx);
        j.Write("status_3xx", ev.metrics.status3xx);
        j.Write("status_4xx", ev.metrics.status4xx);
        j.Write("status_5xx", ev.metrics.status5xx);
        j.Write("connect_failures", ev.metrics.connectFailures);
        j.Write("tls_failures", ev.metrics.tlsFailures);
        j.Write("request_timeouts", ev.metrics.requestTimeouts);
        j.Write("pool_exhausted", ev.metrics.poolExhausted);
        j.Write("other_errors", ev.metrics.otherErrors);
        j.Write("reconnects", ev.metrics.reconnects);
        j.Write("coalesce_hits", ev.metrics.coalesceHits);
        j.Write("bytes_out", ev.metrics.bytesOut);
        j.Write("bytes_in", ev.metrics.bytesIn);
        j.Write("slots_in_use", ev.metrics.slotsInUse);

        if(latencyOn) {
            const auto st = WFX::ComputeLatencyStats(WFX::GetEndpointLatencyAt(e));
            j.Obj("latency");
            j.Write("count", st.count);
            j.Write("mean_us", static_cast<std::uint64_t>(st.meanUs));
            j.Write("p50_us", st.p50Us);
            j.Write("p99_us", st.p99Us);
            j.Write("max_us", st.maxUs);
            j.End();
        }
        j.End();
    }
    j.End();
})
