#include "engine.hpp"

#include "include/http/response.hpp"
#include "http/common/http_error_msgs.hpp"
#include "http/formatters/parser/http_parser.hpp"
#include "http/formatters/serializer/http_serializer.hpp"
#include "http/routing/router.hpp"
#include "shared/apis/master_api.hpp"
#include "utils/backport/string.hpp"
#include "utils/filesystem/filesystem.hpp"
#include "utils/process/process.hpp"

#if defined(__linux__)
    #include <dlfcn.h>
#endif

namespace WFX::Core {

Engine::Engine(const char* dllPath)
    : connHandler_(CreateConnectionHandler())
{
    // Load user's DLL file which we compiled / is cached
    HandleUserDLLInjection(dllPath);

    // Now that user code is available to us, load middleware in proper order
    HandleMiddlewareLoading();
}

void Engine::Listen(const std::string& host, int port)
{
    connHandler_->Initialize(host, port);

    connHandler_->SetReceiveCallback([this](ConnectionContext* ctx){
        this->HandleRequest(ctx);
    });
    connHandler_->Run();
}

void Engine::Stop()
{
    connHandler_->Stop();

    logger_.Info("[Engine]: Stopped Successfully!");
}

// vvv Internals vvv
void Engine::HandleRequest(ConnectionContext* ctx)
{
    // This will be transmitted through all the layers (from here to middleware to user)
    HttpResponse res;
    Response userRes{&res, WFX::Shared::GetHttpAPIV1(), WFX::Shared::GetConfigAPIV1()};

    HttpParseState state = HttpParser::Parse(ctx);

    switch(state)
    {        
        case HttpParseState::PARSE_INCOMPLETE_HEADERS:
        case HttpParseState::PARSE_INCOMPLETE_BODY:
            // ctx->timeoutTick = connHandler_->GetCurrentTick();
            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);
            connHandler_->ResumeReceive(ctx);
            return;
        
        case HttpParseState::PARSE_EXPECT_100:
            // ctx->timeoutTick = connHandler_->GetCurrentTick();
            ctx->SetConnectionState(ConnectionState::CONNECTION_ALIVE);
            connHandler_->Write(ctx, "HTTP/1.1 100 Continue\r\n\r\n");
            return;
        
        case HttpParseState::PARSE_EXPECT_417:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            connHandler_->Write(ctx, "HTTP/1.1 417 Expectation Failed\r\n\r\n");
            return;

        case HttpParseState::PARSE_SUCCESS:
        {
            // Version is important for Serializer to properly create a response
            // HTTP/1.1 and HTTP/2 have different formats dawg
            res.version = ctx->requestInfo->version;

            auto& reqInfo     = ctx->requestInfo;
            auto  conn        = reqInfo->headers.GetHeader("Connection");
            bool  shouldClose = true;

            if(!conn.empty()) {
                res.Set("Connection", std::string{conn});
                shouldClose = StringGuard::CaseInsensitiveCompare(conn, "close");
            }
            else
                res.Set("Connection", "close");
            
            // A bit of shortcut if its public route (starts with '/public/')
            if(StartsWith(reqInfo->path, "/public/")) {
                // Skip the '/public' part (7 chars)
                std::string_view relativePath = reqInfo->path.substr(7); 
                std::string fullRoute = config_.projectConfig.publicDir + std::string(relativePath);

                // Send the file
                res.Status(HttpStatus::OK)
                    .SendFile(std::move(fullRoute), true);
            }
            else {
                // Get the callback for the route we got, if it doesn't exist, we display error
                auto callback = Router::GetInstance().MatchRoute(
                                    reqInfo->method,
                                    reqInfo->path,
                                    reqInfo->pathSegments
                                );
    
                if(!callback)
                    res.Status(HttpStatus::NOT_FOUND).SendText("404: Route not found :(");
                else {
                    // Only execute user callback if middleware chain is successful
                    if(middleware_.ExecuteMiddleware(*reqInfo, userRes))
                        (*callback)(*reqInfo, userRes);
                }
            }

            ctx->parseState = static_cast<std::uint8_t>(HttpParseState::PARSE_IDLE);
            // ctx->timeoutTick = connHandler_->GetCurrentTick();

            HandleResponse(res, ctx, shouldClose);
            return;
        } 

        case HttpParseState::PARSE_ERROR:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            connHandler_->Write(ctx, badRequest);
            return;

        case HttpParseState::PARSE_STREAMING_BODY:
        default:
            ctx->SetConnectionState(ConnectionState::CONNECTION_CLOSE);
            connHandler_->Write(ctx, notImplemented);
            return;
    }
}

void Engine::HandleResponse(HttpResponse& res, ConnectionContext* ctx, bool shouldClose)
{
    auto&& [serializeResult, bodyView] = HttpSerializer::SerializeToBuffer(res, ctx->rwBuffer);

    ConnectionState afterWriteState = shouldClose
                                        ? ConnectionState::CONNECTION_CLOSE
                                        : ConnectionState::CONNECTION_ALIVE;
    
    ctx->SetConnectionState(afterWriteState);

    switch(serializeResult)
    {
        case SerializeResult::SERIALIZE_SUCCESS:
            if(res.IsFileOperation())
                connHandler_->WriteFile(ctx, std::string(bodyView));
            else
                connHandler_->Write(ctx, std::string_view{});

            return;

        case SerializeResult::SERIALIZE_BUFFER_INSUFFICIENT:
            connHandler_->Write(ctx, std::string_view{});
            return;

        default:
            logger_.Error("[Engine]: Failed to serialize response");
            connHandler_->Close(ctx);
            return;
    }
}

// vvv HELPER STUFF vvv
void Engine::HandleUserDLLInjection(const char* dllPath)
{
#if defined(_WIN32)
    // Windows
    HMODULE userModule = LoadLibraryA(dllPath);
    if (!userModule) {
        DWORD err = GetLastError();
        logger_.Fatal("[Engine]: ", dllPath, " was not found. Error: ", err);
        return; // logger_.Fatal probably terminates — keep for clarity
    }

    FARPROC rawProc = GetProcAddress(userModule, "RegisterMasterAPI");
    if (!rawProc) {
        DWORD err = GetLastError();
        logger_.Fatal("[Engine]: Failed to find RegisterMasterAPI() in user DLL. Error: ", err);
        return;
    }

    // Cast to your function type (assumes calling convention matches)
    auto registerFn = reinterpret_cast<WFX::Shared::RegisterMasterAPIFn>(rawProc);
#else
    // POSIX (Linux / macOS / *nix)
    // RTLD_NOW: resolve symbols immediately; RTLD_GLOBAL: let module export symbols globally if needed.
    void* handle = dlopen(dllPath, RTLD_NOW | RTLD_GLOBAL);
    if(!handle) {
        const char* err = dlerror();
        logger_.Fatal("[Engine]: ", dllPath, " dlopen failed: ", (err ? err : "unknown error"));
    }

    // Clear any existing error
    dlerror();
    void* rawSym = dlsym(handle, "RegisterMasterAPI");
    const char* dlsym_err = dlerror();
    if(!rawSym || dlsym_err)
        logger_.Fatal("[Engine]: Failed to find RegisterMasterAPI() in user SO: ",
                      (dlsym_err ? dlsym_err : "symbol not found"));

    auto registerFn = reinterpret_cast<WFX::Shared::RegisterMasterAPIFn>(rawSym);
#endif
    // Call into the user module to inject the API
    registerFn(WFX::Shared::GetMasterAPI());
    logger_.Info("[Engine]: Successfully injected API and initialized user module: ", dllPath);
}

void Engine::HandleMiddlewareLoading()
{
    // // Just for testing, let me register simple middleware
    // middleware_.RegisterMiddleware("Logger", [this](HttpRequest& req, Response& res) {
    //     logger_.Info("[Logger-Middleware]: Request on path: ", req.path);
    //     return true;
    // });

    middleware_.LoadMiddlewareFromConfig(config_.projectConfig.middlewareList);

    // After we load the middleware, we no longer need the map thingy as all the stuff is properly loaded-
    // -inside of middlewareCallbacks_ stack
    // K I L L
    // I T
    middleware_.DiscardFactoryMap();
}

} // namespace WFX