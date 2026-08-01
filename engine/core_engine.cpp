// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "core_engine.hpp"
#include "http/response.hpp"
#include "http/request.hpp"
#include "http/response/http_response.hpp"
#include "http/request/http_request.hpp"
#include "http/connection/http_connection.hpp"
#include "http/common/http_error_msgs.hpp"
#include "http/common/http_master_state.hpp"
#include "http/parser/http_parser.hpp"
#include "shared/apis/master_api.hpp"
#include "utils/string/string.hpp"
#include "utils/fileops/filesystem.hpp"
#include "utils/process/process.hpp"
#include "utils/diagnostics/crash_tracer.hpp"
#include "shared/utils/detection_macro.hpp"
#include "shared/utils/memory.hpp"

#ifdef WFX_PLATFORM_POSIX
#include <dlfcn.h>
#endif

#include <chrono>

namespace WFX::Core {

using namespace WFX::Http;
using namespace WFX::Shared;
using namespace WFX::Utils;

enum ConnectionHeader : std::uint8_t {
    NONE = 0,
    CLOSE = 1 << 0,
    KEEP_ALIVE = 1 << 1,
    UPGRADE = 1 << 2,
    ERROR = 1 << 3,
};

// Monotonic microseconds for route latency. Stamp and read both go through here, so the base-
// -cancels out and only the delta matters
static std::uint64_t NowUs()
{
    auto tse = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(tse).count();
}

// vvv Main Functions vvv
CoreEngine::CoreEngine(const char* dllPath, bool useHttps)
{
    connHandler_ = CreateConnectionHandler(useHttps);
    if(!connHandler_)
        logger_.Fatal("[CoreEngine]: Failed to create connection backend");

    // Initialize API backend before anything else
    InitHttpAPIExt1(&router_, &middleware_);
    InitEndpointAPIExt1(connHandler_.get());
    InitAsyncAPIExt1(connHandler_.get());
    InitUtilsAPIExt1(&router_, connHandler_.get());

    // We set it on our end because each compiled binary has its own copy of 'GlobalWFXApi'
    // If we want it to work on our end, we gotta set it here as well
    SetMasterApi(GetMasterAPI());

    // Load user's DLL file which we compiled / is cached
    HandleUserDLLInjection(dllPath);

    // Now that user code is available to us, load middleware in proper order
    HandleMiddlewareLoading();
}

void CoreEngine::Listen(const std::string& host, std::uint16_t port)
{
    connHandler_->Initialize(host, port);
    connHandler_->SetEngineCallbacks([this](ClientCtx* ctx) { this->HandleRequest(ctx); },
                                     [this](ClientCtx* ctx) { this->HandleClose(ctx); });
    connHandler_->Run();
}

void CoreEngine::Stop()
{
    connHandler_->Stop();
    logger_.Info("[CoreEngine]: Stopped Successfully!");
}

// vvv Internal Functions vvv
void CoreEngine::HandleRequest(ClientCtx* ctx)
{
    WFX_TRACE();

    auto& networkConfig = config_.networkConfig;

    // Allocate response once per connection, reused across requests via Reset()
    if(!ctx->responseInfo)
        ctx->responseInfo = New<HttpResponse>();

    auto& res = *ctx->responseInfo;

    // Main shit
    const HttpParseState state = HttpParser::Parse(ctx);

    switch(state) {
        case HttpParseState::PARSE_INCOMPLETE_HEADERS:
        case HttpParseState::PARSE_INCOMPLETE_BODY:
            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);
            connHandler_->RefreshExpiry(ctx, state == HttpParseState::PARSE_INCOMPLETE_HEADERS
                                                 ? networkConfig.headerTimeout
                                                 : networkConfig.bodyTimeout);
            connHandler_->ResumeReceive(ctx);
            return;

        case HttpParseState::PARSE_EXPECT_100:
            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);
            connHandler_->RefreshExpiry(ctx, networkConfig.bodyTimeout);
            connHandler_->Write(ctx, "HTTP/1.1 100 Continue\r\n\r\n");
            return;

        case HttpParseState::PARSE_EXPECT_417:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            connHandler_->Write(ctx, "HTTP/1.1 417 Expectation Failed\r\n\r\n");
            return;

        case HttpParseState::PARSE_SUCCESS: {
            // After parsing, ctx->trackBytes becomes the compact state register used by-
            // -'HandleSuccess' for async resumption IF needed that is
            // For now reset ctx->trackBytes so ctx->trackAsync becomes zeroed out 'HandleSuccess'
            ctx->trackBytes = 0;

            // Stamp request-dispatch time for route latency, read back in RecordRouteMetrics. Gated-
            // -so the clock read is only paid when latency is on
            if(Utils::MetricTracer::LatencyEnabled())
                ctx->routeStartUs = NowUs();

            auto& reqInfo = *ctx->requestInfo;

            auto connHeader = reqInfo.headers.GetHeader("Connection");
            auto connMask = HandleConnectionHeader(connHeader);

            // RFC violation, close connection
            if(connMask & ConnectionHeader::ERROR) {
                ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
                connHandler_->Write(ctx, HttpError::BAD_REQUEST);
                return;
            }

            // In this case:
            // HTTP/1.0: Defaults to close
            // HTTP/1.1: Defaults to keep-alive
            const bool shouldClose = (connMask == ConnectionHeader::NONE)
                                         ? (reqInfo.version == HttpVersion::HTTP_1_0)
                                         : static_cast<bool>(connMask & ConnectionHeader::CLOSE);

            ctx->SetConnectionState(shouldClose ? ConnectionState::CONNECTION_CLOSE
                                                : ConnectionState::CONNECTION_ALIVE);

            // Wire rwBuffer + version into response before any writes
            // Write buffer allocated once, reused across requests on same connection
            if(!ctx->rwBuffer.IsWriteInitialized() && !ctx->rwBuffer.InitWriteBuffer(networkConfig.maxSendBufferSize)) {
                ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
                connHandler_->Write(ctx, HttpError::INTERNAL_ERROR);
                return;
            }

            res.Reset();
            res.SetRWBuffer(&ctx->rwBuffer);
            res.SetVersion(reqInfo.version);
            res.SetShouldClose(shouldClose);

            // Connection limit is a capacity refusal (503); request limit is an actual rate-
            // -limit (429). Neither is a protocol error, so both go through the normal response-
            // -path and honor shouldClose above, instead of force-closing a connection the client-
            // -asked to keep alive
            switch(AllowRequest(ctx)) {
                case RateLimitResult::CONNECTION_LIMIT:
                    HandleError(ctx, HttpStatus::SERVICE_UNAVAILABLE, "503: Connection limit exceeded");
                    goto __HandleResponse;
                case RateLimitResult::REQUEST_LIMIT:
                    HandleError(ctx, HttpStatus::TOO_MANY_REQUESTS, "429: Rate limit exceeded");
                    goto __HandleResponse;
                default:
                    break;
            }

            // Public file shortcut
            if(reqInfo.path.starts_with("/public/")) {
                const std::string_view relativePath = reqInfo.path.substr(7);
                const std::string fullRoute = config_.projectConfig.publicDir + std::string(relativePath);

                res.SendFile(fullRoute, true);
                goto __HandleResponse;
            }

            {
                auto node = router_.MatchRoute(reqInfo.method, reqInfo.path, reqInfo.pathSegments);
                if(!node) {
                    HandleError(ctx, HttpStatus::NOT_FOUND, "404: Route not found :(");
                    goto __HandleResponse;
                }

                reqInfo.routeNode_ = node;
                HandleSuccess(ctx);
                return;
            }

        __HandleResponse:
            FinishRequest(ctx);
            HandleResponse(ctx);
            return;
        }

        case HttpParseState::PARSE_ERROR:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            connHandler_->Write(ctx, HttpError::BAD_REQUEST);
            return;

        case HttpParseState::PARSE_STREAMING_BODY:
        default:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            connHandler_->Write(ctx, HttpError::NOT_IMPLEMENTED);
            return;
    }
}

void CoreEngine::HandleResponse(ClientCtx* ctx)
{
    WFX_TRACE();

    HttpResponse& res = *ctx->responseInfo;

    // Sanity checks
    if(!res.IsCommitted())
        res.Commit();

    // Every completed request converges here (sync, async, 404, public file), so per-route-
    // -counters are recorded once at this single point
    RecordRouteMetrics(ctx);

    if(res.IsFile()) {
        connHandler_->WriteFile(ctx, res.TakeFilePath());
        return;
    }

    if(res.IsStream()) {
        connHandler_->Stream(ctx, res.TakeGenerator());
        return;
    }

    // 'rwBuffer' already has the full serialized wire response, just write that
    connHandler_->Write(ctx, {});
}

void CoreEngine::HandleSuccess(ClientCtx* ctx)
{
    WFX_TRACE();

    auto* httpApi = GetHttpAPIExt1();
    auto& req = *ctx->requestInfo;
    auto& res = *ctx->responseInfo;
    auto* node = static_cast<const TrieNode*>(req.routeNode_);

    const Response userRes{&res};
    const Request userReq{&req};

    const ExecutionLevel eLevel = ctx->trackAsync.GetELevel();

    if(eLevel == ExecutionLevel::RESPONSE)
        goto __HandleResponse;

    if(eLevel == ExecutionLevel::MIDDLEWARE) {
        auto [success, isAsync, isBroken] = middleware_.ExecuteMiddleware(ctx, node, userReq, userRes);

        if(!success) {
            // Middleware returned MwBreak. Assuming it sent response (it should), finish the request
            if(isBroken)
                goto __HandleResponse;

            if(!isAsync) {
                ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
                HandleError(ctx, HttpStatus::INTERNAL_SERVER_ERROR, "Middleware Execution Failure");
                goto __HandleResponse;
            }

            // Async middleware, coroutine will fire 'OnCoroutineComplete' when done
            FinishRequest(ctx);
            return;
        }

        // Update 'eLevel' to be 'RESPONSE' level so the next time this shits called, we-
        // -directly jump to '__HandleResponse'
        ctx->trackAsync.SetELevel(ExecutionLevel::RESPONSE);
    }

    // Sync, execute it right now
    if(node->callback.kind == CallbackKind::SYNC)
        node->callback.sync(userReq, userRes);

    // Async, check if we have executed it entirely right now, if not-
    // -schedule it for later
    else {
        // Set context (type erased) at http api side before calling async callback
        // And also erase it after callback is done, if the callback hasn't finished, the-
        // -scheduler will set the ptr later on when needed, no need to keep a dangling pointer
        httpApi->setGlobalPtrData(static_cast<void*>(ctx));

        node->callback.async(userReq, userRes, CoreEngine::OnCoroutineComplete, ctx);

        httpApi->setGlobalPtrData(nullptr);

        // If the coroutine already completed synchronously ('final_suspend' already fired the callback),-
        // -the response is already handled
        // If still suspended, it will fire later. Either way, we are done here
        FinishRequest(ctx);
        return;
    }

__HandleResponse:
    FinishRequest(ctx);
    HandleResponse(ctx);
}

// vvv Helper Functions vvv
void CoreEngine::OnCoroutineComplete(void* ud, AsyncResult result)
{
    auto* ctx = static_cast<ClientCtx*>(ud);
    auto* engine = GetMasterState().enginePtr;

    if(result.status != AsyncStatus::COMPLETED) {
        ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
        engine->HandleError(ctx, HttpStatus::INTERNAL_SERVER_ERROR, "Async Failure");
    }

    // If this was middleware, inject the action into the pipeline
    else if(ctx->trackAsync.GetELevel() == ExecutionLevel::MIDDLEWARE) {
        *ctx->trackAsync.GetMAction() = result.action;

        // Re-enter 'HandleSuccess' to run the route or more middlewares
        // This is safe because the current coroutine frame is already destroyed
        engine->HandleSuccess(ctx);
        return;
    }

    // Route completed, serialize and send
    engine->HandleResponse(ctx);
}

void CoreEngine::RecordRouteMetrics(ClientCtx* ctx)
{
    auto& req = *ctx->requestInfo;
    auto& res = *ctx->responseInfo;

    // Unmatched traffic (404, public file) has no route to attribute to, so it is not recorded
    const auto* node = static_cast<const TrieNode*>(req.routeNode_);
    if(!node)
        return;

    auto* rm = MetricTracer::CurrentRoute(node->metricsIdx);
    if(!rm)
        return;

    rm->requests++;

    const auto code = static_cast<std::uint16_t>(res.GetStatus());
    rm->status1xx += (code >= 100 && code < 200);
    rm->status2xx += (code >= 200 && code < 300);
    rm->status3xx += (code >= 300 && code < 400);
    rm->status4xx += (code >= 400 && code < 500);
    rm->status5xx += (code >= 500 && code < 600);

    // dataLength is the fully serialized response for buffered bodies. File and stream bodies-
    // -live outside rwBuffer, so this counts their headers only, the true wire total stays in-
    // -network.bytesWritten
    if(ctx->rwBuffer.IsWriteInitialized())
        if(const auto* wm = ctx->rwBuffer.GetWriteMeta())
            rm->bytesOut += wm->dataLength;

    // routeStartUs stays 0 when latency is off (the stamp is gated), so this also skips the read
    if(ctx->routeStartUs != 0)
        MetricTracer::RecordRouteLatencyUs(node->metricsIdx, NowUs() - ctx->routeStartUs);
}

void CoreEngine::FinishRequest(ClientCtx* ctx)
{
    ctx->SetParseState(HttpParseState::PARSE_IDLE);
    connHandler_->RefreshExpiry(ctx, config_.networkConfig.idleTimeout);
}

void CoreEngine::HandleError(ClientCtx* ctx, HttpStatus code, std::string_view message)
{
    auto& res = *ctx->responseInfo;

    // Reset the should close because it may have changed since
    res.SetShouldClose(ctx->GetConnectionState() == ConnectionState::CONNECTION_CLOSE);
    res.AbortWithError(code, message);
}

std::uint8_t CoreEngine::HandleConnectionHeader(std::string_view header)
{
    std::uint8_t mask = ConnectionHeader::NONE;
    std::size_t start = 0;
    const std::size_t size = header.size();

    while(start < size) {
        // Find comma
        std::size_t end = header.find(',', start);
        if(end == std::string_view::npos)
            end = size;

        // Extract token substring trimming leading and trailing spaces / tabs
        const std::string_view token = StringUtils::TrimView(header.substr(start, end - start));

        // CLOSE
        if(StringUtils::InsensitiveStringCompare(token, "close")) {
            if(mask & ConnectionHeader::KEEP_ALIVE)
                return ConnectionHeader::ERROR; // Mutually exclusive

            mask |= ConnectionHeader::CLOSE;
        }

        // KEEP-ALIVE
        else if(StringUtils::InsensitiveStringCompare(token, "keep-alive")) {
            if(mask & ConnectionHeader::CLOSE)
                return ConnectionHeader::ERROR; // Mutually exclusive

            mask |= ConnectionHeader::KEEP_ALIVE;
        }

        // UPGRADE
        else if(StringUtils::InsensitiveStringCompare(token, "upgrade"))
            mask |= ConnectionHeader::UPGRADE;

        // UNKNOWN
        else
            return ConnectionHeader::ERROR;

        // Move to next token
        start = end + 1;
    }

    return mask;
}

RateLimitResult CoreEngine::AllowRequest(ClientCtx* ctx)
{
    // Resolve + count against ConnectionLimiter once per connection, on its very first request
    if(!ctx->ipAcquired) {
        ctx->connInfo = IpUtils::ResolveClientIp(ctx->connInfo, ctx->requestInfo->headers, config_.ipConfig);

        if(!connectionLimiter_.AllowConnection(ctx->connInfo))
            return RateLimitResult::CONNECTION_LIMIT;

        ctx->ipAcquired = 1;
    }

    // Own bit, own retry: Acquire() can fail on a full tracked-identity cap independently of-
    // -ConnectionLimiter, and that failure is transient, not one-shot like ipAcquired above
    if(!ctx->rateLimiterAcquired) {
        if(!requestRateLimiter_.Acquire(ctx->connInfo))
            return RateLimitResult::REQUEST_LIMIT;

        ctx->rateLimiterAcquired = 1;
    }

    return requestRateLimiter_.AllowRequest(ctx->connInfo) ? RateLimitResult::ALLOWED : RateLimitResult::REQUEST_LIMIT;
}

void CoreEngine::HandleClose(ClientCtx* ctx)
{
    // Both bits are only ever cleared by ClientCtx::Reset(), right after this call returns, on-
    // -slot recycle. Each release is gated on its own bit, mirroring which Acquire() succeeded
    if(ctx->ipAcquired)
        connectionLimiter_.ReleaseConnection(ctx->connInfo);

    if(ctx->rateLimiterAcquired)
        requestRateLimiter_.Release(ctx->connInfo);
}

void CoreEngine::HandleUserDLLInjection(const char* dllPath)
{
    // RTLD_NOW: resolve symbols immediately; RTLD_GLOBAL: let module export symbols globally if needed
    void* handle = dlopen(dllPath, RTLD_NOW | RTLD_GLOBAL);
    if(!handle) {
        const char* err = dlerror();
        logger_.Fatal("[CoreEngine]: ", dllPath, " dlopen failed: ", (err ? err : "unknown error"));
    }

    // Clear any existing error
    dlerror();
    void* rawSym = dlsym(handle, "RegisterMasterAPI");
    const char* dlsymErr = dlerror();
    if(!rawSym || dlsymErr)
        logger_.Fatal("[CoreEngine]: Failed to find RegisterMasterAPI() in user SO. Error: ",
                      (dlsymErr ? dlsymErr : "symbol not found"));

    auto registerFn = reinterpret_cast<RegisterMasterAPIFn>(rawSym);

    // Call into the user module to inject the API
    registerFn(GetMasterAPI());
    logger_.Info("[CoreEngine]: Successfully injected API and initialized user module: ", dllPath);
}

void CoreEngine::HandleMiddlewareLoading()
{
    middleware_.LoadMiddlewareFromConfig(config_.projectConfig.middlewareList);

    // After we load the middleware, we no longer need the map thingy as all the stuff is properly loaded-
    // -inside of middlewareCallbacks_ stack
    // K I L L
    // I T
    middleware_.DiscardFactoryMap();
}

} // namespace WFX::Core