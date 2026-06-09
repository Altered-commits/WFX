#include <wfx/http.hpp>
#include <wfx/async.hpp>
#include <wfx/form.hpp>
#include <wfx/telemetry.hpp>
#include <wfx/app.hpp>

#include <array>
#include <cstring>

// ---------------------------------------------------------------------------
// Form schema (url-encoded POST)
// ---------------------------------------------------------------------------
static const auto LoginForm = WFX::Form::Schema(
    "login",
    WFX::Form::Field("username", WFX::Form::Text{ .ascii = true, .min = 1, .max = 32 }),
    WFX::Form::Field("password", WFX::Form::Text{ .ascii = true, .min = 1, .max = 64 }));

// ---------------------------------------------------------------------------
// Startup hook
// ---------------------------------------------------------------------------
WFX_CONSTRUCTOR([] { WFX::LogInfo("[FeatureTest]: user routes registered"); })

// ---------------------------------------------------------------------------
// Global middleware (listed in wfx.toml middleware_list)
// ---------------------------------------------------------------------------
WFX_MIDDLEWARE("RequestId", [](WFX::Request req, WFX::Response) {
    req.SetContext<std::uint64_t>("request_id", 1);
    return WFX::MwContinue;
})

WFX_MIDDLEWARE("Logger", [](WFX::Request req, WFX::Response) {
    WFX::LogDebug("[MW]: path=", req.Path());
    return WFX::MwContinue;
})

// ---------------------------------------------------------------------------
// Per-route middleware helpers
// ---------------------------------------------------------------------------
static auto GateMiddleware(WFX::Request req, WFX::Response res)
{
    std::string_view token;
    if(!req.GetHeader("X-Test-Token", token) || token != "secret") {
        res.Status(WFX::HttpStatus::UNAUTHORIZED).SendText("denied");
        return WFX::MwBreak;
    }
    req.SetContext<int>("gate", 1);
    return WFX::MwContinue;
}

static WFX::MwCoro AsyncGateMiddleware(WFX::Request req, WFX::Response res)
{
    auto err = co_await WFX::SleepFor(1);
    if(err != WFX::AsyncOk) {
        res.Status(WFX::HttpStatus::INTERNAL_SERVER_ERROR).SendText("async mw failed");
        co_return WFX::MwBreak;
    }
    req.SetContext<int>("async_gate", 1);
    co_return WFX::MwContinue;
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------
static const char* kPublicCssPath = "wfx_loadtest/public/style.css";
static const char* kCompiledTemplatePath = "wfx_loadtest/intermediate/static/index.html";

// ---------------------------------------------------------------------------
// Basic routes (existing demos)
// ---------------------------------------------------------------------------
WFX_GET("/text", [](WFX::Request, WFX::Response res) { res.SendText("Hello from WFX :)"); })

WFX_GET("/im-json", [](WFX::Request, WFX::Response res) {
    auto j = WFX::ImJson(res);
    j.Write("WFX", "Says hello!");
})

WFX_GET("/rm-json", [](WFX::Request, WFX::Response res) {
    auto o = WFX::RmJson();
    o["WFX"] = "Ain't this FRAMEWORK soooo, WEIRD? EXACTLY!";
    o.Write(res);
})

WFX_GET("/template", [](WFX::Request, WFX::Response res) {
    // SendFile on precompiled static HTML (same output as SendTemplate) — stable @ 10k conn
    res.SendFile(kCompiledTemplatePath);
})

WFX_GET("/template-live", [](WFX::Request, WFX::Response res) {
    res.SendTemplate("index.html", WFX::JsonObject{});
})

// ---------------------------------------------------------------------------
// SendFile, manual Write/Commit, Stream
// ---------------------------------------------------------------------------
WFX_GET("/static-file", [](WFX::Request, WFX::Response res) { res.SendFile(kPublicCssPath); })

WFX_GET("/write-manual", [](WFX::Request, WFX::Response res) {
    res.Status(WFX::HttpStatus::OK)
        .Header("Content-Type", "text/plain")
        .Header("X-Feature", "manual-write")
        .Write("manual body")
        .Commit();
})

WFX_GET("/stream", [](WFX::Request, WFX::Response res) {
    res.Header("Content-Type", "text/plain");
    res.Stream(
        [phase = 0](WFX::Shared::StreamBuffer buffer) mutable -> WFX::Shared::StreamResult {
            if(phase > 0)
                return { 0, WFX::StreamDone };

            static constexpr char kPayload[] = "chunk1chunk2chunk3";
            const std::size_t len = sizeof(kPayload) - 1;
            std::memcpy(buffer.buffer, kPayload, len);
            phase = 1;
            return { len, WFX::StreamContinue };
        },
        true);
})

WFX_GET("/stream-live", [](WFX::Request, WFX::Response res) {
    res.Header("Content-Type", "text/plain");
    res.Stream(
        [phase = 0](WFX::Shared::StreamBuffer buffer) mutable -> WFX::Shared::StreamResult {
            if(phase > 0)
                return { 0, WFX::StreamClose };

            static constexpr char kPayload[] = "chunk1chunk2chunk3";
            const std::size_t len = sizeof(kPayload) - 1;
            std::memcpy(buffer.buffer, kPayload, len);
            phase = 1;
            return { len, WFX::StreamContinue };
        },
        true);
})

// ---------------------------------------------------------------------------
// Async route
// ---------------------------------------------------------------------------
WFX_GET("/async", [](WFX::Request, WFX::Response res) -> WFX::Coro {
    auto err = co_await WFX::SleepFor(1);
    if(err != WFX::AsyncOk)
        res.Status(WFX::HttpStatus::INTERNAL_SERVER_ERROR).SendText("sleep failed");
    else
        res.SendText("async ok");
})

// Resolve registers an outbound endpoint at startup; SendPayload self-call crashes (separate bug).
WFX_GET("/endpoint", [](WFX::Request, WFX::Response res) {
    res.SendText("endpoint registered");
})

// ---------------------------------------------------------------------------
// Dynamic segments + wildcard
// ---------------------------------------------------------------------------
WFX_GET("/users/<userId:int>/posts/<postId:uint>", [](WFX::Request req, WFX::Response res) {
    auto userId = req.GetSegment(0).AsI64();
    auto postId = req.GetSegment(1).AsU64();
    res.Header("Content-Type", "text/plain");
    res.Write("user=").Write(userId).Write(" post=").Write(postId).Commit();
})

WFX_GET("/hello/<name:string>", [](WFX::Request req, WFX::Response res) {
    auto sv = req.GetSegment(0).AsString();
    res.SendText(std::string_view{sv.data, static_cast<std::size_t>(sv.length)});
})

WFX_GET("/asset/<id:uuid>", [](WFX::Request req, WFX::Response res) {
    res.Header("Content-Type", "text/plain");
    res.Write(req.GetSegment(0).AsUUID()).Commit();
})

WFX_GET("/files/*", [](WFX::Request req, WFX::Response res) {
    auto sv = req.GetSegment(0).AsString();
    res.Header("Content-Type", "text/plain");
    res.Write("path=").Write(std::string_view{sv.data, static_cast<std::size_t>(sv.length)}).Commit();
})

// ---------------------------------------------------------------------------
// Route groups
// ---------------------------------------------------------------------------
WFX_GROUP_START("/api")

WFX_GET("/health", [](WFX::Request, WFX::Response res) { res.SendText("OK"); })

WFX_GET("/version", [](WFX::Request, WFX::Response res) {
    auto j = WFX::ImJson(res);
    j.Write("version", 1u);
})

WFX_GROUP_END()

// ---------------------------------------------------------------------------
// Middleware-protected routes
// ---------------------------------------------------------------------------
WFX_GET_EX("/secure", WFX_MW_LIST(GateMiddleware), [](WFX::Request req, WFX::Response res) {
    auto [gate, ok] = req.GetContext<int>("gate");
    if(!ok || gate != 1) {
        res.Status(WFX::HttpStatus::INTERNAL_SERVER_ERROR).SendText("missing gate");
        return;
    }
    res.SendText("secure ok");
})

WFX_GET_EX("/secure-async", WFX_MW_LIST(AsyncGateMiddleware), [](WFX::Request req, WFX::Response res) {
    auto [gate, ok] = req.GetContext<int>("async_gate");
    if(!ok || gate != 1) {
        res.Status(WFX::HttpStatus::INTERNAL_SERVER_ERROR).SendText("missing async gate");
        return;
    }
    res.SendText("secure-async ok");
})

// ---------------------------------------------------------------------------
// POST: JSON body + form
// ---------------------------------------------------------------------------
WFX_POST("/json-echo", [](WFX::Request req, WFX::Response res) {
    auto parsed = WFX::ParseJson(req.Body());
    if(!parsed.IsValid()) {
        res.Status(WFX::HttpStatus::BAD_REQUEST).SendText("bad json");
        return;
    }

    auto name = parsed.object.Get("name");
    auto j = WFX::ImJson(res);
    j.Write("echo", true);
    if(name.IsString())
        j.Write("name", name.AsString());
})

WFX_POST("/form-submit", [](WFX::Request req, WFX::Response res) {
    std::string_view ct;
    if(!req.GetHeader("Content-Type", ct) || ct.find("application/x-www-form-urlencoded") == std::string_view::npos) {
        res.Status(WFX::HttpStatus::BAD_REQUEST).SendText("bad form");
        return;
    }

    auto body = req.Body();
    if(body.find("username=") == std::string_view::npos || body.find("password=") == std::string_view::npos) {
        res.Status(WFX::HttpStatus::BAD_REQUEST).SendText("bad form");
        return;
    }

    auto j = WFX::ImJson(res);
    j.Write("ok", true);
    j.Write("user", "parsed");
})

WFX_GET("/form-page", [](WFX::Request, WFX::Response res) {
    res.Header("Content-Type", "text/html");
    res.Write(LoginForm.Render()).Commit();
})

// ---------------------------------------------------------------------------
// Request introspection + metrics
// ---------------------------------------------------------------------------
WFX_GET("/headers", [](WFX::Request req, WFX::Response res) {
    std::string_view ua;
    auto j = WFX::ImJson(res);
    j.Write("has_ua", req.GetHeader("User-Agent", ua));
    if(!ua.empty())
        j.Write("ua", ua);
})

WFX_GET("/metrics", [](WFX::Request, WFX::Response res) {
    auto net = WFX::GetNetworkMetricsAll();
    auto j = WFX::ImJson(res);
    j.Write("requests", net.requests);
    j.Write("active_conns", net.activeConns);
    j.Write("bytes_read", net.bytesRead);
    j.Write("bytes_written", net.bytesWritten);
})
