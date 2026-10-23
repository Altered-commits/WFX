// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits
//
// Every route exists to give base_audit.py a surface to attack.
// Covers every user-facing WFX feature: dynamic segments (uint/int/string/uuid),
// per-route middleware (continue/break/skip-next, sync and async), context storage,
// async handlers, callables sharing a type (plain functions, one factory's closures),
// JSON (immediate + retained + parsing + merge), chained headers,
// group-prefixed paths, templates, forms, query params, CORS, Stream(), the Flush API,
// SendFile downloads, deliberate contract violations, and metrics.

#include <wfx/http.hpp>
#include <wfx/memory.hpp>
#include <wfx/telemetry.hpp>
#include <wfx/form.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

// Basic routes
WFX_GET("/health", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

WFX_GET("/text", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

WFX_GET("/echo", [](WFX::Request req, WFX::Response res) {
    std::string_view v;
    if(!req.GetHeader("X-Echo", v))
        v = "(none)";

    res.Status(200).Header("X-Echoed", v).SendText(v);
})

WFX_POST("/echo-body", [](WFX::Request req, WFX::Response res) { res.Status(200).SendText(req.Body()); })

// Echoes back method, path, and an arbitrary request header alongside the body, all in
// one response - request.Path()/GetHeader()/Body() are all string_views into the
// connection's read buffer, so a single big request here exercises whether every one of
// them survives a mid-parse RWBuffer relocation (see PrepareForBody in
// http/parser/http_parser.cpp), not just the body
WFX_POST("/echo-full", [](WFX::Request req, WFX::Response res) {
    std::string_view marker;
    if(!req.GetHeader("X-Marker", marker))
        marker = "(none)";

    const char* methodStr = "UNKNOWN";
    switch(req.Method()) {
        case WFX::HttpMethod::GET: methodStr = "GET"; break;
        case WFX::HttpMethod::POST: methodStr = "POST"; break;
        case WFX::HttpMethod::PUT: methodStr = "PUT"; break;
        case WFX::HttpMethod::DELETE: methodStr = "DELETE"; break;
        case WFX::HttpMethod::PATCH: methodStr = "PATCH"; break;
        case WFX::HttpMethod::HEAD: methodStr = "HEAD"; break;
        case WFX::HttpMethod::OPTIONS: methodStr = "OPTIONS"; break;
        case WFX::HttpMethod::CONNECT: methodStr = "CONNECT"; break;
        case WFX::HttpMethod::TRACE: methodStr = "TRACE"; break;
        default: break;
    }

    res.Status(200)
        .Header("X-Echo-Method", methodStr)
        .Header("X-Echo-Path", req.Path())
        .Header("X-Echo-Marker", marker)
        .SendText(req.Body());
})

// Same path, three methods, no OPTIONS handler: the generic OPTIONS Allow: fallback
// (CoreEngine::HandleGenericOptions) has real methods to report here instead of an empty set
WFX_GET("/cors/multi", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("get"); })
WFX_POST("/cors/multi", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("post"); })
WFX_PUT("/cors/multi", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("put"); })

// Query-string parsing: GetQueryParams()/QueryParams::Get(). Always looks up "v" and also
// reports how many pairs the parser found overall, so one request can assert on both a specific
// key's raw (undecoded) value and how the whole set got parsed (duplicate keys, empty segments
// from stray '&'s collapsing, etc.). truncated says whether the query carried more pairs than
// the cap parsed, which is how a key reading as absent is told apart from one never sent
WFX_GET("/query/echo", [](WFX::Request req, WFX::Response res) {
    auto qp = req.GetQueryParams();

    std::string_view v;
    const bool present = qp.Get("v", v);

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("present", present);
    j.Write("len", static_cast<std::uint64_t>(present ? v.size() : 0));
    j.Write("value", present ? v : std::string_view{});
    j.Write("count", static_cast<std::uint64_t>(qp.Count()));
    j.Write("truncated", qp.Truncated());
})

// The same through the inline form, which holds its pairs in the handler rather than on the heap.
// Sized deliberately small so one request can show the caller's own cap taking effect
WFX_GET("/query/fixed", [](WFX::Request req, WFX::Response res) {
    auto qp = req.GetQueryParams<4>();

    std::string_view v;
    const bool present = qp.Get("v", v);

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("present", present);
    j.Write("value", present ? v : std::string_view{});
    j.Write("count", static_cast<std::uint64_t>(qp.Count()));
    j.Write("truncated", qp.Truncated());
})

WFX_GET("/big", [](WFX::Request, WFX::Response res) {
    static const WFX::String blob(1u << 20, 'A');
    res.Status(200).SendText(blob);
})

WFX_GET("/stream", [](WFX::Request, WFX::Response res) {
    int remaining = 512;
    res.Status(200).Stream([remaining](WFX::StreamBuffer buf) mutable -> WFX::StreamResult {
        if(remaining <= 0)
            return {0, WFX::StreamDone};
        std::size_t n = std::min<std::size_t>(buf.size, 256);
        std::memset(buf.buffer, 'S', n);
        remaining--;
        return {n, WFX::StreamContinue};
    });
})

// Awaitable outbound chunk flush: FlushStart()/co_await Flush()/co_await FlushEnd(). Unlike
// /stream (a synchronous generator the engine drives after the handler returns), these run
// mid-coroutine, so a handler can co_await an upstream fetch between rounds. No real upstream
// here, but the framing is identical either way, that's the whole point of the primitive.

// One Write() then FlushEnd() directly, no separate Flush() call: exercises the path where the
// final round's body and the chunk terminator both go out in the same PrepareFlushChunk(true) call
WFX_GET("/flush/single", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    res.Status(200);
    res.FlushStart();
    res.Write("single-round-payload");
    co_await res.FlushEnd();
    co_return;
})

// FlushEnd() with nothing ever written: an immediate zero-body chunked response
WFX_GET("/flush/zero", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    res.Status(200);
    res.FlushStart();
    co_await res.FlushEnd();
    co_return;
})

// 200 rounds of "<index>\n", each its own Flush(), then a FlushEnd() with no data of its own
// (empty final chunk after real ones): exercises repeated gap-reserve/backfill across many rounds.
// The status is checked rather than dropped, since a client that leaves partway through should end
// the loop rather than have it write another 200 chunks into a connection that is closing
WFX_GET("/flush/multi", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    res.Status(200);
    res.FlushStart();

    for(int i = 0; i < 200; i++) {
        res.Write(i);
        res.Write("\n");

        if(co_await res.Flush() == WFX::FlushFailed)
            co_return;
    }

    co_await res.FlushEnd();
    co_return;
})

// 4000 rounds x 2048 bytes (~8 MiB), each round a repeating letter tied to its index: large enough
// that a slow reader forces real EAGAIN/backpressure through DrainWriteBuffer/ResumeFlushChunk,
// not just the synchronous fast path every other Flush test above stays on
WFX_GET("/flush/heavy", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    res.Status(200);
    res.FlushStart();

    char block[2048];
    for(int i = 0; i < 4000; i++) {
        std::memset(block, 'A' + (i % 26), sizeof(block));
        res.Write(std::string_view{block, sizeof(block)});
        co_await res.Flush();
    }

    co_await res.FlushEnd();
    co_return;
})

// Deliberately forgets FlushEnd(): regression coverage for the Commit() path that used to leave
// an unbackfilled chunk-header gap (10 raw zero bytes) on the wire when a handler returns without
// finalizing. Must now still produce a validly chunk-terminated (if truncated) response
WFX_GET("/flush/no-end", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    res.Status(200);
    res.FlushStart();
    res.Write("first");
    co_await res.Flush();
    res.Write("second");
    co_await res.Flush();
    co_return;
})

WFX_GET("/violate/flush-after-body", [](WFX::Request, WFX::Response res) {
    res.Status(200).Write("body-first");
    res.FlushStart();
})

WFX_GET("/violate/flush-twice", [](WFX::Request, WFX::Response res) {
    res.Status(200);
    res.FlushStart();
    res.FlushStart();
})

WFX_GET("/download", [](WFX::Request req, WFX::Response res) {
    std::string_view f;
    if(!req.GetHeader("X-File", f)) {
        res.Status(400).SendText("missing X-File");
        return;
    }
    WFX::String path = "public/";
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

// Echoes the requested status code back, so the metrics phase can drive each response
// status class (2xx / 3xx / 4xx / 5xx) through one route and check the per-class buckets.
WFX_GET("/status/<code:uint>", [](WFX::Request req, WFX::Response res) {
    const auto code = static_cast<std::uint16_t>(req.GetSegment(0).AsU64());
    res.Status(code).SendText("s");
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
    j.Write("active_conns", net.activeClientConns);
    j.Write("bytes_written", net.bytesWritten);
    j.End();

    j.Obj("process");
    j.Write("rss_bytes", self.rssBytes);
    j.Write("restarts", self.restarts);
    j.Write("crashes", self.crashes);
    j.End();

    // Per-route counters, summed across workers, each tagged with its own path and method.
    // rv.path / HttpMethodToStringView return WFX::StringView, written directly via the
    // JsonWriter StringView overload (no manual std::string_view wrapping).
    const bool latencyOn = WFX::MetricsLatencyEnabled();
    j.Write("latency_enabled", latencyOn);

    j.Arr("routes");
    for(std::uint16_t r = 0; r < WFX::RouteMetricCount(); r++) {
        const auto rv = WFX::GetRouteMetricsAt(r);
        j.Obj();
        j.Write("path", rv.path);
        j.Write("method", WFX::HttpMethodToStringView(rv.method));
        j.Write("requests", rv.metrics.requests);
        j.Write("status_1xx", rv.metrics.status1xx);
        j.Write("status_2xx", rv.metrics.status2xx);
        j.Write("status_3xx", rv.metrics.status3xx);
        j.Write("status_4xx", rv.metrics.status4xx);
        j.Write("status_5xx", rv.metrics.status5xx);
        j.Write("bytes_out", rv.metrics.bytesOut);

        if(latencyOn) {
            const auto st = WFX::ComputeLatencyStats(WFX::GetRouteLatencyAt(r));
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

// Dynamic segment routes
// Segment index 0 is always the first (and only) dynamic capture because
// static path components are consumed but never pushed to outParams.
// :uint  -> unsigned 64-bit integer
WFX_GET("/items/<id:uint>", [](WFX::Request req, WFX::Response res) {
    auto seg = req.GetSegment(0);
    res.Status(200).Write(seg.AsU64()).Commit();
})

// :int  -> signed 64-bit integer (path may include '-')
WFX_GET("/items/signed/<id:int>", [](WFX::Request req, WFX::Response res) {
    auto seg = req.GetSegment(0);
    res.Status(200).Write(seg.AsI64()).Commit();
})

// :string -> arbitrary path component
WFX_GET("/greet/<name:string>", [](WFX::Request req, WFX::Response res) {
    auto seg = req.GetSegment(0);
    auto sv = seg.AsString();
    res.Status(200).Write("hello ").Write(std::string_view{sv.Data(), sv.Size()}).Commit();
})

// :uuid -> 8-4-4-4-12 UUID
WFX_GET("/uuid/<id:uuid>", [](WFX::Request req, WFX::Response res) {
    auto seg = req.GetSegment(0);
    res.Status(200).Write(seg.AsUUID()).Commit();
})

// Two sibling parameter routes at one depth, differing only in the capture's type and the static
// tail. A numeric segment parses as both <id:uint> and <tag:string>, so the router must still match
// the right tail and hand each handler its own single capture at index 0 rather than pushing one
// segment per sibling and shifting the index. Each handler reads GetSegment(0) as its own type
WFX_GET("/route-shift/<id:uint>/end-a", [](WFX::Request req, WFX::Response res) {
    res.Status(200).Write(req.GetSegment(0).AsU64()).Commit();
})
WFX_GET("/route-shift/<tag:string>/end-b", [](WFX::Request req, WFX::Response res) {
    auto sv = req.GetSegment(0).AsString();
    res.Status(200).Write(std::string_view{sv.Data(), sv.Size()}).Commit();
})


// Group-prefixed paths, via WFX_GROUP_START/WFX_GROUP_END

WFX_GROUP_START("/api/v1")

WFX_GET("/status", [](WFX::Request, WFX::Response res) { res.Status(200).SendText("ok"); })

WFX_GET("/item/<id:uint>", [](WFX::Request req, WFX::Response res) {
    auto seg = req.GetSegment(0);
    res.Status(200).Write(seg.AsU64()).Commit();
})

WFX_GROUP_END()


// Per-route middleware routes
// MwContinue: middleware adds a header, handler runs normally.
// NOTE: Header() in middleware calls EnsureHeadersOpen() which flushes the
// status line (default 200) immediately, so the handler must NOT call
// Status() again, only write the body.
WFX_GET_EX("/mw/injected", WFX_MW_LIST([](WFX::Request, WFX::Response res) {
               res.Header("X-Route-MW", "hit");
               return WFX::MwContinue;
           }),
           [](WFX::Request, WFX::Response res) { res.Header("Content-Type", "text/plain").Write("ok").Commit(); })

// MwBreak: middleware sends 403 and aborts the chain, handler never runs
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

// Callables that share a type: plain functions of one signature, and closures from one middleware
// factory. Each route must still run its own
static void FnSyncA(WFX::Request, WFX::Response res)
{
    res.Status(200).SendText("fn-sync-a");
}

static void FnSyncB(WFX::Request, WFX::Response res)
{
    res.Status(200).SendText("fn-sync-b");
}

static WFX::Coro FnAsyncA(WFX::Request, WFX::Response res)
{
    res.Status(200).SendText("fn-async-a");
    co_return;
}

static WFX::Coro FnAsyncB(WFX::Request, WFX::Response res)
{
    res.Status(200).SendText("fn-async-b");
    co_return;
}

static auto TagMw(const char* tag)
{
    return [tag](WFX::Request, WFX::Response res) {
        res.Header("X-Mw-Tag", tag);
        return WFX::MwContinue;
    };
}

WFX_GET("/fn/sync-a", FnSyncA)
WFX_GET("/fn/sync-b", FnSyncB)
WFX_GET("/fn/async-a", FnAsyncA)
WFX_GET("/fn/async-b", FnAsyncB)

WFX_GET_EX("/mw/factory-a", WFX_MW_LIST(TagMw("a")),
           [](WFX::Request, WFX::Response res) { res.Header("Content-Type", "text/plain").Write("ok").Commit(); })
WFX_GET_EX("/mw/factory-b", WFX_MW_LIST(TagMw("b")),
           [](WFX::Request, WFX::Response res) { res.Header("Content-Type", "text/plain").Write("ok").Commit(); })

// Async middleware that finishes without ever suspending, the way an auth check deciding on what
// the request already carries would. Its coroutine completes inside the call that starts it.
WFX_GET_EX("/mw/async-inline", WFX_MW_LIST([](WFX::Request, WFX::Response res) -> WFX::MwCoro {
               res.Header("X-Async-MW", "hit");
               co_return WFX::MwContinue;
           }),
           [](WFX::Request, WFX::Response res) { res.Header("Content-Type", "text/plain").Write("async-mw-ok").Commit(); })

// Same, but it rejects: sends 403 and breaks the chain before the handler
WFX_GET_EX("/mw/async-inline-break", WFX_MW_LIST([](WFX::Request, WFX::Response res) -> WFX::MwCoro {
               res.Status(403).SendText("async-blocked");
               co_return WFX::MwBreak;
           }),
           [](WFX::Request, WFX::Response res) { res.Status(200).SendText("unreachable"); })

// Async route that responds and returns without awaiting anything
WFX_GET("/async/inline", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    res.Status(200).SendText("inline");
    co_return;
})

// Async handler that exercises the coroutine + timer path under load
WFX_GET("/async/sleep", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    auto s = co_await WFX::SleepFor(25); // 25 ms
    if(s != WFX::AsyncOk) {
        res.Status(500).SendText("timer failed");
        co_return;
    }
    res.Status(200).SendText("slept");
    co_return;
})

// Child coroutines that throw before ever reaching co_return, so they have no value to hand back.
// Their exception has to reach the co_awaiting parent as an exception, like any other call's
static WFX::Task<WFX::String> ThrowingChild()
{
    throw std::runtime_error("child failed");
    co_return WFX::String{"unreachable"};
}

static WFX::Task<> ThrowingVoidChild()
{
    throw std::runtime_error("void child failed");
    co_return;
}

WFX_GET("/async/child-throws", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    try {
        const WFX::String value = co_await ThrowingChild();
        res.Status(200).SendText(std::string_view{value});
    }
    catch(const std::runtime_error&) {
        res.Status(200).SendText("caught");
    }
    co_return;
})

// Nobody catches the child's exception here, so the handler fails as a whole (500) rather than
// carrying on as if the child had succeeded
WFX_GET("/async/void-child-throws", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    co_await ThrowingVoidChild();
    res.Status(200).SendText("continued");
    co_return;
})

// Async handler that stays suspended long enough for a client to send its next request on the
// same connection before this one is answered
WFX_GET("/async/slow", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    auto s = co_await WFX::SleepFor(1000); // 1 s
    if(s != WFX::AsyncOk) {
        res.Status(500).SendText("timer failed");
        co_return;
    }
    res.Status(200).SendText("slow-done");
    co_return;
})

// Async middleware that stays suspended the same way, in front of a sync route
WFX_GET_EX("/mw/async-slow", WFX_MW_LIST([](WFX::Request, WFX::Response res) -> WFX::MwCoro {
               auto s = co_await WFX::SleepFor(1000); // 1 s
               if(s != WFX::AsyncOk) {
                   res.Status(500).SendText("timer failed");
                   co_return WFX::MwBreak;
               }
               co_return WFX::MwContinue;
           }),
           [](WFX::Request, WFX::Response res) { res.Status(200).SendText("mw-slow-done"); })

// A helper coroutine the route below awaits, so a suspended request is a two-frame chain: a client
// that leaves mid-sleep has the engine kill the route's frame and this one together
static WFX::Coro NestedSleep()
{
    co_await WFX::SleepFor(1000); // 1 s
}

WFX_GET("/async/nested-slow", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    co_await NestedSleep();
    res.Status(200).SendText("nested-done");
    co_return;
})

// JSON routes
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
    w.Arr("tags");
    w.Push("admin"); // a string literal, which has to stay a string rather than turn into a bool
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

// RmJson Merge and Set(key, JsonRef) taking values from another object. The source is destroyed
// before the target is written, so anything the target still shares with it shows up as freed
// memory or as the wrong bytes
WFX_GET("/json/merge", [](WFX::Request, WFX::Response res) {
    auto dst = WFX::RmJson();
    dst["keep"] = (std::uint64_t)1;
    {
        auto src = WFX::RmJson();
        src["name"] = std::string_view{"src-owned"};
        src["nested"]["inner"] = std::string_view{"deep"};
        src["list"].PushBack((std::uint64_t)7);
        src["list"].PushBack(std::string_view{"eight"});

        dst.Merge(src);
        dst["root"].Set("alias", src["nested"]);
    }

    res.Status(200);
    dst.Write(res);
})

// RmJson scalars assigned over an existing object or array. Each container's buffer has to be
// freed rather than lost with the pointer to it, which the exit-time leak scan would catch
WFX_GET("/json/overwrite", [](WFX::Request, WFX::Response res) {
    auto o = WFX::RmJson();
    o["num"]["a"] = (std::uint64_t)1;
    o["str"].PushBack((std::uint64_t)2);
    o["nil"]["b"]["c"] = true;

    o["num"] = (std::uint64_t)5;
    o["str"] = std::string_view{"text"};
    o["nil"] = nullptr;

    res.Status(200);
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


// Chained header test that verifies the chainable Response API
WFX_GET("/chain", [](WFX::Request, WFX::Response res) {
    res.Status(200)
        .Header("X-Chain-A", "alpha")
        .Header("X-Chain-B", "beta")
        .Header("X-Chain-C", "gamma")
        .SendText("chain");
})


// Template routes
//
//  /template/static    -> static template (no dynamic tags, file-served path)
//  /template/dynamic   -> var tags: simple value, uint, nested attr (meta.version)
//  /template/cond/<n>  -> if / elif / else / endif (existence-based branches)
//  /template/loop      -> for / endfor over a string array
//  /template/include   -> {% include 'frag.html' %} + var
//  /template/inherit   -> {% extends 'base.html' %} + {% block %} override with var
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

// Form routes, the surface for the forms security audit (percent-decoding,
// Content-Type matching, field-order/count structure, validator/sanitizer
// bounds, and value-into-header injection)
static const auto AuditForm = WFX::Form::Schema("audit",
    WFX::Form::Field("username", WFX::Form::Text{{}, true, 3, 32}),
    WFX::Form::Field("email", WFX::Form::Email{}),
    WFX::Form::Field("age", WFX::Form::UInt{{}, 0, 120}),
    WFX::Form::Field("bio", WFX::Form::Text{{false}, false, 0, 500}));

// Single optional field, generous max: isolates percent-decoding / structural
// edge cases from the multi-field validators above
static const auto RawForm =
    WFX::Form::Schema("raw", WFX::Form::Field("v", WFX::Form::Text{{false}, false, 0, 8192}));

static int StatusForFormError(WFX::Form::FormError err)
{
    if(err == WFX::Form::BadContentType)
        return 415;
    if(err == WFX::Form::Malformed)
        return 400;
    return 422; // CleanFailed
}

WFX_POST("/form", [](WFX::Request req, WFX::Response res) {
    decltype(AuditForm)::CleanedType data;
    auto err = AuditForm.Parse(req, data);

    if(err != WFX::Form::Ok) {
        res.Status(StatusForFormError(err)).SendText("form_error");
        return;
    }

    auto& username = std::get<0>(data);
    auto& email = std::get<1>(data);
    auto& age = std::get<2>(data);
    auto& bio = std::get<3>(data);

    // Reflect the decoded value into a header verbatim: proves the pipeline as a
    // whole (percent-decode -> header write) can't be used for response splitting,
    // regardless of whether the form module itself defends against it
    res.Status(200).Header("X-Form-Username", username.value);

    auto j = WFX::ImJson(res);
    j.Write("ok", true);
    j.Write("username", username.value);
    j.Write("email", email.value);
    j.Write("age", static_cast<std::int64_t>(age.value));
    j.Write("bio_present", bio.present);
    j.Write("bio_len", static_cast<std::uint64_t>(bio.present ? bio.value.size() : 0));
})

WFX_POST("/form/raw", [](WFX::Request req, WFX::Response res) {
    decltype(RawForm)::CleanedType data;
    auto err = RawForm.Parse(req, data);

    if(err != WFX::Form::Ok) {
        res.Status(StatusForFormError(err)).SendText("form_error");
        return;
    }

    auto& v = std::get<0>(data);

    res.Status(200);
    auto j = WFX::ImJson(res);
    j.Write("present", v.present);
    j.Write("len", static_cast<std::uint64_t>(v.present ? v.value.size() : 0));
    j.Write("value", v.present ? v.value : std::string_view{});
})
