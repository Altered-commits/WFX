// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_CORE_ENGINE_HPP
#define WFX_CORE_ENGINE_HPP

#include "config/config.hpp"
#include "http/connection/http_connection_factory.hpp"
#include "http/limits/connection_limiter.hpp"
#include "http/limits/request_rate_limiter.hpp"
#include "http/middleware/http_middleware.hpp"
#include "http/routing/router.hpp"

#include <string>

namespace WFX::Core {

// ALLOWED continues the request; the other two tell HandleRequest which error to write
enum class RateLimitResult : std::uint8_t { ALLOWED, CONNECTION_LIMIT, REQUEST_LIMIT };

class CoreEngine {
public: // Main Stuff
    CoreEngine(const char* dllPath, bool useHttps);
    void Listen(const std::string& host, std::uint16_t port);
    void Stop();

public: // Static stuff
    static void OnCoroutineComplete(void* ud, Shared::AsyncResult result);

private: // Internal Functions
    void HandleRequest(Http::ClientCtx* ctx);
    void HandleResponse(Http::ClientCtx* ctx);
    void HandleSuccess(Http::ClientCtx* ctx);

private: // Helper Functions
    void RecordRouteMetrics(Http::ClientCtx* ctx);
    void FinishRequest(Http::ClientCtx* ctx);
    void HandleError(Http::ClientCtx* ctx, Shared::HttpStatus code, std::string_view message);
    std::uint8_t HandleConnectionHeader(std::string_view header);
    void HandleUserDLLInjection(const char* dllDir);
    void HandleMiddlewareLoading();
    RateLimitResult AllowRequest(Http::ClientCtx* ctx);
    void HandleClose(Http::ClientCtx* ctx);

private:
    Config& config_ = GetConfig();
    Utils::Logger& logger_ = Utils::GetLogger();
    Shared::WorkerMetrics* metrics_ = Utils::MetricTracer::Current();

    Http::HttpMiddleware middleware_;
    Http::Router router_;

    Http::ConnectionLimiter connectionLimiter_;
    Http::RequestRateLimiter requestRateLimiter_;

    std::unique_ptr<Http::HttpConnectionHandler> connHandler_;
};

} // namespace WFX::Core

#endif // WFX_CORE_ENGINE_HPP