#include "core_engine.hpp"

#include "async/interface.hpp"
#include "http/response.hpp"
#include "http/common/http_error_msgs.hpp"
#include "http/formatters/parser/http_parser.hpp"
#include "http/formatters/serializer/http_serializer.hpp"
#include "shared/apis/master_api.hpp"
#include "utils/backport/string.hpp"
#include "utils/fileops/filesystem.hpp"
#include "utils/process/process.hpp"

#if defined(__linux__)
    #include <dlfcn.h>
#endif

namespace WFX::Core {

// Some internal enum stuff for connection header
enum ConnectionHeader : std::uint8_t {
    NONE       = 0,
    CLOSE      = 1 << 0,
    KEEP_ALIVE = 1 << 1,
    UPGRADE    = 1 << 2,
    ERROR      = 1 << 3,
};

// vvv Main Functions vvv
CoreEngine::CoreEngine(const char* dllPath, bool useHttps)
{
    connHandler_ = CreateConnectionHandler(useHttps);
    if(!connHandler_)
        logger_.Fatal("[CoreEngine]: Failed to create connection backend");

    // Initialize API backend before anything else
    WFX::Shared::InitHttpAPIV1(&router_, &middleware_);
    WFX::Shared::InitAsyncAPIV1(connHandler_.get());

    // Load user's DLL file which we compiled / is cached
    HandleUserDLLInjection(dllPath);

    // Now that user code is available to us, load middleware in proper order
    HandleMiddlewareLoading();
}

void CoreEngine::Listen(const std::string& host, int port)
{
    connHandler_->Initialize(host, port);

    connHandler_->SetEngineCallbacks(
        [this](ConnectionContext* ctx) {
            this->HandleRequest(ctx);
        },
        [this](ConnectionContext* ctx) {
            bool inResponse = (ctx->trackAsync.GetELevel() == ExecutionLevel::RESPONSE);
            const TrieNode* route = nullptr;

            if(!inResponse) {
                const TrieNode* const* ptr =
                    ctx->requestInfo->GetContext<const TrieNode*>("__IntrnlCtx_RouteNode");

                if(!ptr || !*ptr)
                    logger_.Fatal(
                        "[CoreEngine]: RouteNode context missing/null inside 'CompletionCallback'"
                    );

                route = *ptr;
            }

            this->HandleSuccess(ctx, route);
        }
    );
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
    // This will be transmitted through all the layers (from here to middleware to user)
    if(!ctx->responseInfo)
        ctx->responseInfo = new HttpResponse{};

    auto& res           = *ctx->responseInfo;
    auto& networkConfig = config_.networkConfig;

    // Main shit
    HttpParseState state = HttpParser::Parse(ctx);

    switch(state) {
        case HttpParseState::PARSE_INCOMPLETE_HEADERS:
        case HttpParseState::PARSE_INCOMPLETE_BODY:
            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);
            connHandler_->RefreshExpiry(ctx, state == HttpParseState::PARSE_INCOMPLETE_HEADERS ?
                                            networkConfig.headerTimeout : networkConfig.bodyTimeout);
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

        case HttpParseState::PARSE_SUCCESS:
        {
            // After parsing, ctx->trackBytes becomes the compact state register used by-
            // -'HandleSuccess' for async resumption IF needed that is
            // For now reset ctx->trackBytes so ctx->trackAsync becomes zeroed out 'HandleSuccess'
            ctx->trackBytes = 0;

            // Version is important for Serializer to properly create a response
            // HTTP/1.1 and HTTP/2 have different formats duh
            res.version = ctx->requestInfo->version;

            auto& reqInfo    = *ctx->requestInfo;
            auto  connHeader = reqInfo.headers.GetHeader("Connection");
            auto  connMask   = HandleConnectionHeader(connHeader);

            // RFC violation, close connection
            if(connMask & ConnectionHeader::ERROR) {
                ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
                connHandler_->Write(ctx, HttpError::badRequest);
                return;
            }

            bool shouldClose = false;

            // In this case:
            // HTTP/1.0: Defaults to close
            // HTTP/1.1: Defaults to keep-alive
            if(connMask == ConnectionHeader::NONE)
                shouldClose = (reqInfo.version == HttpVersion::HTTP_1_0);

            // Propagate value from request header
            else
                shouldClose = connMask & ConnectionHeader::CLOSE;

            // Set the 'connection' header ourselves in final response
            res.Set("Connection", shouldClose ? "close" : "keep-alive");

            // Set the connection state according to this right now, later if we have any issue, it-
            // -will be overridden
            ctx->SetConnectionState(
                shouldClose
                ? ConnectionState::CONNECTION_CLOSE
                : ConnectionState::CONNECTION_ALIVE
            );

            // A bit of shortcut if its public route (starts with '/public/')
            if(StartsWith(reqInfo.path, "/public/")) {
                // Skip the '/public' part (7 chars)
                std::string_view relativePath = reqInfo.path.substr(7); 
                std::string fullRoute = config_.projectConfig.publicDir + std::string(relativePath);

                // Send the file
                res.Status(HttpStatus::OK)
                    .SendFile(std::move(fullRoute), true);
            }
            else {
                // Get the callback for the route we got, if it doesn't exist, we display error
                auto node = router_.MatchRoute(
                                    reqInfo.method,
                                    reqInfo.path,
                                    reqInfo.pathSegments
                                );

                if(!node) {
                    res.Status(HttpStatus::NOT_FOUND)
                        .SendText("404: Route not found :(");
                    goto __HandleResponse;
                }

                // Before handing of control to 'HandleSuccess', setup ctx->trackAsync state for use
                auto& trackAsync = ctx->trackAsync;
                trackAsync.SetMType(MiddlewareType::LINEAR);

                HandleSuccess(ctx, node);

                // One important thing btw, 'HandleSuccess' requires 'node', which uk is fine
                // The issue is, scheduler will not have access to 'node' nor it can query it (even if it can-
                // -its just not performant). So we set req.context to contain this 'node' IF AND ONLY IF we haven't-
                // -reached ExecutionLevel::RESPONSE, cuz till then 'node' is still required by 'HandleSuccess'
                if(trackAsync.GetELevel() != ExecutionLevel::RESPONSE)
                    reqInfo.SetContext("__IntrnlCtx_RouteNode", node);

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
    HttpResponse& res = *ctx->responseInfo;

    auto&& [serializeResult, bodyView] = HttpSerializer::SerializeToBuffer(res, ctx->rwBuffer);

    switch(serializeResult) {
        case SerializeResult::SERIALIZE_SUCCESS:
            if(res.IsFileOperation())
                connHandler_->WriteFile(ctx, std::move(bodyView));
            else if(res.IsStreamOperation())
                connHandler_->Stream(
                    ctx, std::move(std::get<StreamGenerator>(res.body)),
                    res.GetOperation() == OperationType::STREAM_CHUNKED
                );
            else
                connHandler_->Write(ctx, {});

            return;

        // TODO: For insufficient cases, we need to be able to stream the remaining response
        case SerializeResult::SERIALIZE_BUFFER_INSUFFICIENT:
            connHandler_->Write(ctx, {});
            return;

        default:
            logger_.Error("[CoreEngine]: Failed to serialize response");
            connHandler_->Close(ctx);
            return;
    }
}

void CoreEngine::HandleSuccess(ConnectionContext* ctx, const TrieNode* node)
{
    auto* httpApi = WFX::Shared::GetHttpAPIV1();
    auto& req     = *ctx->requestInfo;
    auto& res     = *ctx->responseInfo;

    Response userRes{&res, httpApi};

    // Trackers
    ExecutionLevel eLevel = ctx->trackAsync.GetELevel();

    if(eLevel == ExecutionLevel::RESPONSE)
        goto __HandleResponse;

    if(eLevel == ExecutionLevel::MIDDLEWARE) {
        auto [success, ptr] = middleware_.ExecuteMiddleware(node, req, userRes, ctx, {});

        if(!success) {
            // For failure, just handle response and be done with
            if(!ptr)
                goto __HandleResponse;

            // Its async, let scheduler do its job at the backend
            FinishRequest(ctx);
            return;
        }
        // Update 'eLevel' to be 'RESPONSE' level so the next time this shits called, we-
        // -directly jump to '__HandleResponse'
        ctx->trackAsync.SetELevel(ExecutionLevel::RESPONSE);
    }

    // Sync, execute it right now
    if(auto* sync = std::get_if<SyncCallbackType>(&node->callback))
        (*sync)(req, userRes);

    // Async, check if we have executed it entirely right now, if not-
    // -schedule it for later
    else {
        auto& async = std::get<AsyncCallbackType>(node->callback);

        // Set context (type erased) at http api side before calling async callback
        // And also erase it after callback is done, if the callback hasn't finished, the-
        // -scheduler will set the ptr later on when needed, no need to keep a dangling pointer
        httpApi->SetGlobalPtrData(static_cast<void*>(ctx));

        auto coro = async(req, userRes);
        if(!coro)
            logger_.Fatal("[CoreEngine]: Null coroutine detected in active connection context, aborting");

        coro->Resume();

        // Reset to remove any dangling references
        httpApi->SetGlobalPtrData(nullptr);

        // (Async path)
        if(!coro->IsFinished()) {
            FinishRequest(ctx);
            return;
        }

        // So if coroutine is done, then the user defined lambda should be the only thing-
        // -existing inside of 'ctx->coroStack'. If anything else remains, thats a big no no
        if(ctx->coroStack.size() > 1)
            logger_.Fatal(
                "[CoreEngine]: Coroutine stack imbalance detected after async user callback execution, aborting"
            );
    }

__HandleResponse:
    FinishRequest(ctx);
    HandleResponse(ctx);
}

// vvv Helper Functions vvv
void CoreEngine::FinishRequest(ConnectionContext* ctx)
{
    ctx->SetParseState(HttpParseState::PARSE_IDLE);
    connHandler_->RefreshExpiry(ctx, config_.networkConfig.idleTimeout);
}

std::uint8_t CoreEngine::HandleConnectionHeader(std::string_view header)
{
    std::uint8_t mask  = ConnectionHeader::NONE;
    std::size_t  start = 0;
    std::size_t  size  = header.size();

    while(start < size) {
        // Find comma
        std::size_t end = header.find(',', start);
        if(end == std::string_view::npos)
            end = size;

        // Extract token substring trimming leading and trailing spaces / tabs
        std::string_view token = TrimView(header.substr(start, end - start));

        // CLOSE
        if(StringCanonical::InsensitiveStringCompare(token, "close")) {
            if(mask & ConnectionHeader::KEEP_ALIVE)
                return ConnectionHeader::ERROR; // Mutually exclusive

            mask |= ConnectionHeader::CLOSE;
        }

        // KEEP-ALIVE
        else if(StringCanonical::InsensitiveStringCompare(token, "keep-alive")) {
            if(mask & ConnectionHeader::CLOSE)
                return ConnectionHeader::ERROR; // Mutually exclusive

            mask |= ConnectionHeader::KEEP_ALIVE;
        }

        // UPGRADE
        else if(StringCanonical::InsensitiveStringCompare(token, "upgrade"))
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
    auto registerFn = reinterpret_cast<WFX::Shared::RegisterMasterAPIFn>(rawProc);
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

    auto registerFn = reinterpret_cast<WFX::Shared::RegisterMasterAPIFn>(rawSym);
#endif
    // Call into the user module to inject the API
    registerFn(WFX::Shared::GetMasterAPI());
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

} // namespace WFX