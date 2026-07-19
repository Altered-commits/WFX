// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// TLS audit target. Forces WFX's OUTBOUND HttpEndpoint onto the TLS path
// (EpTlsRequire) against a small, hostile TLS mock — the surface the plaintext
// endpoint audit never exercises.
//
// Five endpoints, ports fixed to match tls_audit.py:
//   good        8443  valid, mkcert-trusted cert   -> must succeed
//   selfsigned  8444  untrusted self-signed cert   -> must refuse
//   wronghost   8445  trusted CA, SAN=evil.example  -> must refuse (hostname)
//   expired     8446  trusted CA, expired           -> must refuse
//   tls12       8447  valid cert, server capped 1.2  -> must refuse (client requires 1.3)
//   fast        8443  same as 'good', 5s budget      -> timeout tests
//
// /call   X-Ep=<persona> X-Path=<p>  -> Ep.Get(p), reflects { ep,status,bodylen,body }
// /inject X-Inject=path|header, body -> feeds the serializer hostile CR/LF/NUL bytes

#include <wfx/http.hpp>
#include <wfx/memory.hpp>
#include <wfx/endpoint/http.hpp>

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>

static WFX::HttpEndpointConfig TlsCfg(std::uint16_t reqTimeout = 10)
{
    return WFX::HttpEndpointConfig{
        .connLimit             = 4,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = reqTimeout,
        .tlsConfig             = WFX::EpTlsRequire,
    };
}

inline const auto Ep_good       = WFX::HttpEndpoint{"127.0.0.1:8443", TlsCfg()};
inline const auto Ep_selfsigned = WFX::HttpEndpoint{"127.0.0.1:8444", TlsCfg()};
inline const auto Ep_wronghost  = WFX::HttpEndpoint{"127.0.0.1:8445", TlsCfg()};
inline const auto Ep_expired    = WFX::HttpEndpoint{"127.0.0.1:8446", TlsCfg()};
inline const auto Ep_tls12      = WFX::HttpEndpoint{"127.0.0.1:8447", TlsCfg()};
inline const auto Ep_fast       = WFX::HttpEndpoint{"127.0.0.1:8443", TlsCfg(5)};

static const WFX::HttpEndpoint* EndpointOf(std::string_view e) noexcept
{
    if(e == "selfsigned") return &Ep_selfsigned;
    if(e == "wronghost")  return &Ep_wronghost;
    if(e == "expired")    return &Ep_expired;
    if(e == "tls12")      return &Ep_tls12;
    if(e == "fast")       return &Ep_fast;
    return &Ep_good;
}

// ---------------------------------------------------------------------------
// In-band TLS upgrade (SlotHandle::UpgradeToTLS)
//
// A deliberately tiny raw protocol, present only to test what happens ACROSS the
// plaintext -> TLS trust boundary. The cert-free upgrade vectors (downgrade
// refusal, garbage handshake) live in tests/endpoint_audit; the two here both
// need a real handshake, which is why they are in this suite.
//
//   plaintext:  -> "STARTTLS\n"        <- "S\n"   (a hostile mock may append
//                                                  attacker plaintext here)
//   [upgrade]
//   TLS:        -> "AUTH\n"            <- "OK <connId>\n"
//               -> "GET <key>\n"       <- "VAL <connId>:<key>\n"
//
// The mock's plaintext injection claims connId 9999. If the engine carried those
// pre-upgrade bytes across the boundary, onConnect would read 9999 as the
// authenticated server's reply; a correct engine discards them and sees the real
// id sent over TLS. That difference is the whole test (CVE-2011-0411 class).
#define UPGRADE_UPSTREAM "127.0.0.1:8448"

namespace {

struct UpReq {
    WFX::String key;
};

struct UpRes {
    WFX::String value;
    std::uint64_t connId = 0;
};

struct UpSlotState {
    std::uint64_t connId = 0;
};

void* UpCreateSlotState(void*)
{
    return WFX::New<UpSlotState>();
}
void UpDestroySlotState(void* s)
{
    WFX::Delete(static_cast<UpSlotState*>(s));
}
void* UpCreateOutput(void*)
{
    return WFX::New<UpRes>();
}
void UpDestroyOutput(void* o)
{
    WFX::Delete(static_cast<UpRes*>(o));
}

WFX::EpCoro UpConnect(WFX::SlotHandle h, void* slotStateVoid)
{
    auto* st = static_cast<UpSlotState*>(slotStateVoid);

    static const char kProbe[] = "STARTTLS\n";
    if(co_await h.Send(kProbe, sizeof(kProbe) - 1) != WFX::EpSlotOk)
        co_return WFX::EpFatal;

    auto answer = co_await h.Receive();
    if(answer.status != WFX::EpSlotOk)
        co_return WFX::EpFatal;

    if(!std::string_view{answer.buf, answer.len}.starts_with("S"))
        co_return WFX::EpFatal;

    if(co_await h.UpgradeToTLS() != WFX::EpSlotOk)
        co_return WFX::EpFatal;

    // Everything from here on must come from the authenticated TLS peer
    static const char kAuth[] = "AUTH\n";
    if(co_await h.Send(kAuth, sizeof(kAuth) - 1) != WFX::EpSlotOk)
        co_return WFX::EpFatal;

    auto recv = co_await h.Receive();
    if(recv.status != WFX::EpSlotOk)
        co_return WFX::EpFatal;

    std::string_view ok{recv.buf, recv.len};
    if(!ok.starts_with("OK"))
        co_return WFX::EpFatal;

    ok.remove_prefix(2);
    while(!ok.empty() && ok.front() == ' ')
        ok.remove_prefix(1);

    std::uint64_t id = 0;
    for(char c : ok) {
        if(c < '0' || c > '9')
            break;
        id = id * 10 + static_cast<std::uint64_t>(c - '0');
    }
    st->connId = id;

    co_return WFX::EpReady;
}

WFX::Shared::SerializeResult UpSerialize(void*, const void* reqVoid, char* buf, std::uint32_t bufLen,
                                         std::uint32_t* written, std::uint64_t*)
{
    auto& req = *static_cast<const UpReq*>(reqVoid);

    const int n = std::snprintf(buf, bufLen, "GET %s\n", req.key.c_str());
    if(n < 0 || static_cast<std::uint32_t>(n) >= bufLen)
        return WFX::EpSerBufferTooSmall;

    *written = static_cast<std::uint32_t>(n);
    return WFX::EpSerOk;
}

WFX::Shared::ParseResult UpParse(void* slotStateVoid, void*, const char* buf, std::uint32_t len,
                                 std::uint32_t* consumed, void* outObj, bool isEof, std::uint64_t*)
{
    auto* st = static_cast<UpSlotState*>(slotStateVoid);
    auto* out = static_cast<UpRes*>(outObj);

    std::string_view view{buf, len};
    const auto nl = view.find('\n');
    if(nl == std::string_view::npos) {
        *consumed = 0;
        return isEof ? WFX::EpParseError : WFX::EpParseIncomplete;
    }

    std::string_view line = view.substr(0, nl);
    *consumed = static_cast<std::uint32_t>(nl + 1);

    if(!line.starts_with("VAL "))
        return WFX::EpParseError;

    line.remove_prefix(4);
    out->value.assign(line.data(), line.size());
    out->connId = st->connId;

    return WFX::EpParseDone;
}

} // namespace

inline const auto Ep_upgrade = WFX::Endpoint<UpReq, UpRes, &UpConnect>{
    UPGRADE_UPSTREAM,
    WFX::EndpointDesc{
        .serialize        = UpSerialize,
        .parse            = UpParse,
        .createSlotState  = UpCreateSlotState,
        .destroySlotState = UpDestroySlotState,
        .createOutput     = UpCreateOutput,
        .destroyOutput    = UpDestroyOutput,
    },
    // Plaintext at connect time: the engine must NOT auto-wrap, the protocol
    // decides when to upgrade. Auto-wrapping would make UpgradeToTLS fail with
    // EpSlotInvalidState, which is itself asserted below
    WFX::EndpointConfig{
        .connLimit             = 2,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 10,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsInsecure,
    }
};

// Same protocol pointed at the always-TLS 'good' listener with tlsConfig=Require,
// so the engine wraps at connect time and onConnect's UpgradeToTLS is a
// double-wrap attempt. That must be refused, not leak the first SSL object.
inline const auto Ep_upgrade_double = WFX::Endpoint<UpReq, UpRes, &UpConnect>{
    "127.0.0.1:8443",
    WFX::EndpointDesc{
        .serialize        = UpSerialize,
        .parse            = UpParse,
        .createSlotState  = UpCreateSlotState,
        .destroySlotState = UpDestroySlotState,
        .createOutput     = UpCreateOutput,
        .destroyOutput    = UpDestroyOutput,
    },
    WFX::EndpointConfig{
        .connLimit             = 1,
        .connectTimeoutSeconds = 5,
        .requestTimeoutSeconds = 10,
        .idleTimeoutSeconds    = 5,
        .maxReconnectAttempts  = 1,
        .tlsConfig             = WFX::EpTlsRequire,
    }
};

WFX_GET("/health", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

// X-Mode double -> the already-secure slot, anything else -> the real upgrade
WFX_GET("/upgrade", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view mode = "normal", key = "hello";
    req.GetHeader("X-Mode", mode);
    req.GetHeader("X-Key", key);

    const auto run = [&](auto& ep) { return ep.SendPayload(UpReq{WFX::String(key.data(), key.size())}); };

    auto [status, out] = mode == "double" ? co_await run(Ep_upgrade_double) : co_await run(Ep_upgrade);

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("ep", static_cast<std::uint64_t>(static_cast<unsigned>(status)));
    if(status == WFX::EpOk) {
        j.Write("value", std::string_view{out->value.data(), out->value.size()});
        j.Write("conn", out->connId);
    }

    co_return;
})

// `st` checked BEFORE `out` is dereferenced — out is empty on any transport/TLS failure.
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
    }
}

WFX_GET("/call", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view epName = "good", path = "/ok";
    req.GetHeader("X-Ep", epName);
    req.GetHeader("X-Path", path);

    auto pr = co_await EndpointOf(epName)->Get(path);
    Emit(req, res, pr.first, pr.second);
    co_return;
})

// Serializer injection probe (over the good TLS endpoint). CR/LF/NUL in the path or
// header must be refused, TLS or not.
WFX_POST("/inject", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    std::string_view mode = "path";
    req.GetHeader("X-Inject", mode);
    std::string_view raw = req.Body();

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
        r.path = raw;
    }

    auto pr = co_await Ep_good.Send(std::move(r));
    Emit(req, res, pr.first, pr.second);
    co_return;
})
