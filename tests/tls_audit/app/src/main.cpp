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
#include <wfx/endpoint/http.hpp>

#include <cstdint>
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

WFX_GET("/health", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

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
