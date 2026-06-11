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

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

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

// vvv Main Functions vvv
CoreEngine::CoreEngine(const char* dllPath, bool useHttps)
{
    connHandler_ = CreateConnectionHandler(useHttps);
    if(!connHandler_)
        logger_.Fatal("[CoreEngine]: Failed to create connection backend");

    // Initialize API backend before anything else
    Shared::InitHttpAPIExt1(connHandler_.get(), &router_, &middleware_);
    Shared::InitAsyncAPIExt1(connHandler_.get());

    // We set it on our end because each compiled binary has its own copy of '__WFXApi'
    // If we want it to work on our end, we gotta set it here as well
    SetMasterApi(Shared::GetMasterAPI());

    // Load user's DLL file which we compiled / is cached
    HandleUserDLLInjection(dllPath);

    // Now that user code is available to us, load middleware in proper order
    HandleMiddlewareLoading();
}

void CoreEngine::Listen(const std::string& host, std::uint16_t port)
{
    connHandler_->Initialize(host, port);

    connHandler_->SetEngineCallback([this](ConnectionContext* ctx) { this->HandleRequest(ctx); });
    connHandler_->Run();
}

void CoreEngine::Stop()
{
    connHandler_->Stop();
    logger_.Info("[CoreEngine]: Stopped Successfully!");
}

// vvv Internal Functions vvv
void CoreEngine::HandleRequest(ConnectionContext* ctx)
{
    WFX_TRACE();

    auto& networkConfig = config_.networkConfig;

    // Allocate response once per connection, reused across requests via Reset()
    if(!ctx->responseInfo)
        ctx->responseInfo = new HttpResponse{};

    auto& res = *ctx->responseInfo;

    // Main shit
    HttpParseState state = HttpParser::Parse(ctx);

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
            metrics_->network.requests++;

            // After parsing, ctx->trackBytes becomes the compact state register used by-
            // -'HandleSuccess' for async resumption IF needed that is
            // For now reset ctx->trackBytes so ctx->trackAsync becomes zeroed out 'HandleSuccess'
            ctx->trackBytes = 0;

            auto& reqInfo = *ctx->requestInfo;
            auto connHeader = reqInfo.headers.GetHeader("Connection");
            auto connMask = HandleConnectionHeader(connHeader);

            // RFC violation, close connection
            if(connMask & ConnectionHeader::ERROR) {
                ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
                connHandler_->Write(ctx, HttpError::badRequest);
                return;
            }

            // In this case:
            // HTTP/1.0: Defaults to close
            // HTTP/1.1: Defaults to keep-alive
            bool shouldClose = (connMask == ConnectionHeader::NONE)
                                   ? (reqInfo.version == HttpVersion::HTTP_1_0)
                                   : static_cast<bool>(connMask & ConnectionHeader::CLOSE);

            ctx->SetConnectionState(shouldClose ? ConnectionState::CONNECTION_CLOSE
                                                : ConnectionState::CONNECTION_ALIVE);

            // Wire rwBuffer + version into response before any writes
            // Write buffer allocated once, reused across requests on same connection
            if(!ctx->rwBuffer.IsWriteInitialized() && !ctx->rwBuffer.InitWriteBuffer(networkConfig.maxSendBufferSize)) {
                ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
                connHandler_->Write(ctx, HttpError::internalError);
                return;
            }

            res.Reset();
            res.SetRWBuffer(&ctx->rwBuffer);
            res.SetVersion(reqInfo.version);
            res.SetShouldClose(shouldClose);

            // Public file shortcut
            if(reqInfo.path.starts_with("/public/")) {
                std::string_view relativePath = reqInfo.path.substr(7);
                std::string fullRoute = config_.projectConfig.publicDir + std::string(relativePath);

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
            connHandler_->Write(ctx, HttpError::badRequest);
            return;

        case HttpParseState::PARSE_STREAMING_BODY:
        default:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            connHandler_->Write(ctx, HttpError::notImplemented);
            return;
    }
}

void CoreEngine::HandleResponse(ConnectionContext* ctx)
{
    WFX_TRACE();

    HttpResponse& res = *ctx->responseInfo;

    // Sanity checks
    if(!res.IsCommitted())
        res.Commit();

    // Metrics
    auto code = static_cast<std::uint16_t>(res.GetStatus());
    metrics_->network.response1xx += (code >= 100 && code < 200);
    metrics_->network.response2xx += (code >= 200 && code < 300);
    metrics_->network.response3xx += (code >= 300 && code < 400);
    metrics_->network.response4xx += (code >= 400 && code < 500);
    metrics_->network.response5xx += (code >= 500 && code < 600);

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

void CoreEngine::HandleSuccess(ConnectionContext* ctx)
{
    WFX_TRACE();

    auto* httpApi = Shared::GetHttpAPIExt1();
    auto& req = *ctx->requestInfo;
    auto& res = *ctx->responseInfo;
    auto* node = static_cast<const TrieNode*>(req.routeNode_);

    Response userRes{&res};
    Request userReq{&req};

    ExecutionLevel eLevel = ctx->trackAsync.GetELevel();

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
        httpApi->SetGlobalPtrData(static_cast<void*>(ctx));

        node->callback.async(userReq, userRes, CoreEngine::OnCoroutineComplete, ctx);

        httpApi->SetGlobalPtrData(nullptr);

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
    auto* ctx = static_cast<ConnectionContext*>(ud);
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

void CoreEngine::FinishRequest(ConnectionContext* ctx)
{
    ctx->SetParseState(HttpParseState::PARSE_IDLE);
    connHandler_->RefreshExpiry(ctx, config_.networkConfig.idleTimeout);
}

void CoreEngine::HandleError(ConnectionContext* ctx, Shared::HttpStatus code, std::string_view message)
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
    std::size_t size = header.size();

    while(start < size) {
        // Find comma
        std::size_t end = header.find(',', start);
        if(end == std::string_view::npos)
            end = size;

        // Extract token substring trimming leading and trailing spaces / tabs
        std::string_view token = StringUtils::TrimView(header.substr(start, end - start));

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

void CoreEngine::HandleUserDLLInjection(const char* dllPath)
{
#if defined(_WIN32)
    // Windows
    HMODULE userModule = LoadLibraryA(dllPath);
    if(!userModule) {
        DWORD err = GetLastError();
        logger_.Fatal("[CoreEngine]: ", dllPath, " was not found. Error: ", err);
        return;
    }

    FARPROC rawProc = GetProcAddress(userModule, "RegisterMasterAPI");
    if(!rawProc) {
        DWORD err = GetLastError();
        logger_.Fatal("[CoreEngine]: Failed to find RegisterMasterAPI() in user DLL. Error: ", err);
        return;
    }

    // Cast to your function type
    auto registerFn = reinterpret_cast<Shared::RegisterMasterAPIFn>(rawProc);
#else
    // POSIX (Linux / macOS / *nix)
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

    auto registerFn = reinterpret_cast<Shared::RegisterMasterAPIFn>(rawSym);
#endif
    // Call into the user module to inject the API
    registerFn(Shared::GetMasterAPI());
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