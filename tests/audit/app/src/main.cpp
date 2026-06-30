// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// Torture target. Every route exists to give torture.py a surface to attack.
// Covers every user-facing WFX feature: dynamic segments (uint/int/string/uuid),
// per-route middleware (continue/break), context storage, async handlers,
// JSON (immediate + retained + parsing), chained headers,
// group-prefixed paths, deliberate contract violations, and metrics.

#include <wfx/http.hpp>
#include <wfx/telemetry.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// Original routes (kept exactly as-is)
// ─────────────────────────────────────────────────────────────────────────────

WFX_GET("/health", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

WFX_GET("/text", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

WFX_GET("/echo", [](WFX::Request req, WFX::Response res) {
    std::string_view v;
    if(!req.GetHeader("X-Echo", v))
        v = "(none)";

    res.Status(200).Header("X-Echoed", v).SendText(v);
})

WFX_POST("/echo-body", [](WFX::Request req, WFX::Response res) { res.Status(200).SendText(req.Body()); })

WFX_GET("/big", [](WFX::Request, WFX::Response res) {
    static const std::string blob(1u << 20, 'A');
    res.Status(200).SendText(blob);
})

WFX_GET("/stream", [](WFX::Request, WFX::Response res) {
    int remaining = 512;
    res.Status(200).Stream([remaining](WFX::Shared::StreamBuffer buf) mutable -> WFX::Shared::StreamResult {
        if(remaining <= 0)
            return {0, WFX::Shared::StreamAction::STOP_AND_ALIVE_CONN};
        std::size_t n = std::min<std::size_t>(buf.size, 256);
        std::memset(buf.buffer, 'S', n);
        remaining--;
        return {n, WFX::Shared::StreamAction::CONTINUE};
    });
})

WFX_GET("/download", [](WFX::Request req, WFX::Response res) {
    std::string_view f;
    if(!req.GetHeader("X-File", f)) {
        res.Status(400).SendText("missing X-File");
        return;
    }
    std::string path = "public/";
    path.append(f);
    res.SendFile(path, true);
})

WFX_GET("/violate/204body",
        [](WFX::Request, WFX::Response res) { res.Status(204).SendText("body on a 204 is forbidden"); })
WFX_GET("/violate/conn", [](WFX::Request, WFX::Response res) {
    res.Status(200).Header("Connection", "close").SendText("engine-owned header");
})
WFX_GET("/violate/recommit", [](WFX::Request, WFX::Response res) {
    res.Status(200).SendText("first");
    res.Write("after commit").Commit();
})

WFX_GET("/metrics", [](WFX::Request, WFX::Response res) {
    auto log = WFX::GetLogMetricsAll();
    auto net = WFX::GetNetworkMetricsAll();
    auto self = WFX::GetProcessMetricsAll();

    res.Status(200);
    auto j = WFX::ImJson(res);

    j.Obj("log");
    j.Write("warn", log.warn);
    j.Write("error", log.error);
    j.Write("fatal", log.fatal);
    j.End();

    j.Obj("network");
    j.Write("accepts", net.accepts);
    j.Write("requests", net.requests);
    j.Write("active_conns", net.activeConns);
    j.Write("response_2xx", net.response2xx);
    j.Write("response_4xx", net.response4xx);
    j.Write("response_5xx", net.response5xx);
    j.End();

    j.Obj("process");
    j.Write("rss_bytes", self.rssBytes);
    j.Write("restarts", self.restarts);
    j.Write("crashes", self.crashes);
    j.End();
})

// ─────────────────────────────────────────────────────────────────────────────
// Dynamic segment routes
// Segment index 0 is always the first (and only) dynamic capture because
// static path components are consumed but never pushed to outParams.
// ─────────────────────────────────────────────────────────────────────────────

// :uint  — unsigned 64-bit integer
WFX_GET("/items/<id:uint>", [](WFX::Request req, WFX::Response res) {
    auto seg = req.GetSegment(0);
    res.Status(200).Write(seg.AsU64()).Commit();
})

// :int  — signed 64-bit integer (path may include '-')
WFX_GET("/items/signed/<id:int>", [](WFX::Request req, WFX::Response res) {
    auto seg = req.GetSegment(0);
    res.Status(200).Write(seg.AsI64()).Commit();
})

// :string — arbitrary path component
WFX_GET("/greet/<name:string>", [](WFX::Request req, WFX::Response res) {
    auto seg = req.GetSegment(0);
    auto sv = seg.AsString();
    res.Status(200).Write("hello ").Write(std::string_view{sv.Data(), sv.Size()}).Commit();
})

// :uuid — 8-4-4-4-12 UUID
WFX_GET("/uuid/<id:uuid>", [](WFX::Request req, WFX::Response res) {
    auto seg = req.GetSegment(0);
    res.Status(200).Write(seg.AsUUID()).Commit();
})

// ─────────────────────────────────────────────────────────────────────────────
// Group-prefixed paths (flat, no WFX_GROUP_START — uses literal prefixes)
// ─────────────────────────────────────────────────────────────────────────────

WFX_GET("/api/v1/status", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

WFX_GET("/api/v1/item/<id:uint>", [](WFX::Request req, WFX::Response res) {
    auto seg = req.GetSegment(0);
    res.Status(200).Write(seg.AsU64()).Commit();
})

// ─────────────────────────────────────────────────────────────────────────────
// Per-route middleware routes
// ─────────────────────────────────────────────────────────────────────────────

// MwContinue: middleware adds a header, handler runs normally.
// NOTE: Header() in middleware calls EnsureHeadersOpen() which flushes the
// status line (default 200) immediately — so the handler must NOT call
// Status() again, only write the body.
WFX_GET_EX("/mw/injected", WFX_MW_LIST([](WFX::Request, WFX::Response res) {
               res.Header("X-Route-MW", "hit");
               return WFX::MwContinue;
           }),
           [](WFX::Request, WFX::Response res) { res.Header("Content-Type", "text/plain").Write("ok").Commit(); })

// MwBreak: middleware sends 403 and aborts the chain — handler never runs
WFX_GET_EX("/mw/blocked", WFX_MW_LIST([](WFX::Request, WFX::Response res) {
               res.Status(403).SendText("blocked");
               return WFX::MwBreak;
           }),
           [](WFX::Request, WFX::Response res) { res.Status(200).SendText("unreachable"); })

// Context storage: middleware sets "uid" = 42, handler reads and echoes it
WFX_GET_EX("/ctx", WFX_MW_LIST([](WFX::Request req, WFX::Response) {
               req.SetContext("uid", (std::uint64_t)42);
               return WFX::MwContinue;
           }),
           [](WFX::Request req, WFX::Response res) {
               auto [uid, ok] = req.GetContext<std::uint64_t>("uid");
               if(!ok) {
                   res.Status(500).SendText("ctx missing");
                   return;
               }
               res.Status(200).Write(uid).Commit();
           })

// MwSkipNext: first MW skips second MW, handler still runs
WFX_GET_EX("/mw/skipnext",
           WFX_MW_LIST([](WFX::Request, WFX::Response) { return WFX::MwSkipNext; },
                       [](WFX::Request, WFX::Response res) {
                           res.Header("X-Should-Not-Appear", "yes");
                           return WFX::MwContinue;
                       }),
           [](WFX::Request, WFX::Response res) { res.Status(200).SendText("handler-ran"); })

// ─────────────────────────────────────────────────────────────────────────────
// Async handler — exercises the coroutine + timer path under load
// ─────────────────────────────────────────────────────────────────────────────

WFX_GET("/async/sleep", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    auto s = co_await WFX::SleepFor(25); // 25 ms
    if(s != WFX::AsyncOk) {
        res.Status(500).SendText("timer failed");
        co_return;
    }
    res.Status(200).SendText("slept");
    co_return;
})

// ─────────────────────────────────────────────────────────────────────────────
// JSON routes
// ─────────────────────────────────────────────────────────────────────────────

// Immediate-mode JSON (zero-heap streaming into response buffer)
WFX_GET("/json/im", [](WFX::Request, WFX::Response res) {
    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("type", "immediate");
    w.Write("value", (std::uint64_t)1234);
    w.Obj("meta");
    w.Write("version", (std::uint64_t)1);
    w.Write("ok", true);
    w.End();
})

// Retained-mode JSON (DOM, then serialized in one shot)
WFX_GET("/json/rm", [](WFX::Request, WFX::Response res) {
    res.Status(200);
    auto o = WFX::RmJson();
    o["type"] = "retained";
    o["value"] = (std::uint64_t)5678;
    o["meta"]["version"] = (std::uint64_t)1;
    o["meta"]["ok"] = true;
    o.Write(res);
})

// JSON body parsing: echoes "name" (string) and "version" (uint) from POST body
WFX_POST("/parse-json", [](WFX::Request req, WFX::Response res) {
    auto result = WFX::ParseJson(req.Body(), true);
    if(!result.IsValid()) {
        res.Status(400).SendText("bad json");
        return;
    }

    auto name = result.object["name"].AsString();
    auto version = result.object["version"].AsUInt();

    res.Status(200);
    auto w = WFX::ImJson(res);
    w.Write("name", name);
    w.Write("version", version);
})

// ─────────────────────────────────────────────────────────────────────────────
// Chained header test — verifies the chainable Response API
// ─────────────────────────────────────────────────────────────────────────────

WFX_GET("/chain", [](WFX::Request, WFX::Response res) {
    res.Status(200)
        .Header("X-Chain-A", "alpha")
        .Header("X-Chain-B", "beta")
        .Header("X-Chain-C", "gamma")
        .SendText("chain");
})

// ─────────────────────────────────────────────────────────────────────────────
// Template routes — full coverage of every WFX template feature
//
//  /template/static    — static template (no dynamic tags, file-served path)
//  /template/dynamic   — var tags: simple value, uint, nested attr (meta.version)
//  /template/cond/<n>  — if / elif / else / endif (existence-based branches)
//  /template/loop      — for / endfor over a string array
//  /template/include   — {% include 'frag.html' %} + var
//  /template/inherit   — {% extends 'base.html' %} + {% block %} override with var
// ─────────────────────────────────────────────────────────────────────────────

WFX_GET("/template/static", [](WFX::Request, WFX::Response res) { res.SendTemplate("index.html", WFX::RmJson()); })

WFX_GET("/template/dynamic", [](WFX::Request, WFX::Response res) {
    auto ctx = WFX::RmJson();
    ctx["title"] = "WFX Rendered";
    ctx["count"] = (std::uint64_t)42;
    ctx["meta"]["version"] = (std::uint64_t)3;
    res.SendTemplate("dynamic.html", std::move(ctx));
})

// n=2 → ctx has "a" → {% if a %} branch ("high")
// n=1 → ctx has "b" only → {% elif b %} branch ("medium")
// n=0 → neither → {% else %} branch ("low")
WFX_GET("/template/cond/<n:uint>", [](WFX::Request req, WFX::Response res) {
    auto ctx = WFX::RmJson();
    auto n = req.GetSegment(0).AsU64();
    if(n >= 2)
        ctx["a"] = true;
    else if(n == 1)
        ctx["b"] = true;
    res.SendTemplate("cond.html", std::move(ctx));
})

WFX_GET("/template/loop", [](WFX::Request, WFX::Response res) {
    auto ctx = WFX::RmJson();
    ctx["items"].PushBack("alpha");
    ctx["items"].PushBack("beta");
    ctx["items"].PushBack("gamma");
    res.SendTemplate("loop.html", std::move(ctx));
})

WFX_GET("/template/include", [](WFX::Request, WFX::Response res) {
    auto ctx = WFX::RmJson();
    ctx["message"] = "hello include";
    res.SendTemplate("page.html", std::move(ctx));
})

WFX_GET("/template/inherit", [](WFX::Request, WFX::Response res) {
    auto ctx = WFX::RmJson();
    ctx["page_title"] = "My Page";
    res.SendTemplate("child.html", std::move(ctx));
})
