#include "core_engine.hpp"

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

CoreEngine::CoreEngine(const char* dllPath, bool useHttps)
    : connHandler_(CreateConnectionHandler(useHttps))
{
    // Load user's DLL file which we compiled / is cached
    HandleUserDLLInjection(dllPath);

    // Now that user code is available to us, load middleware in proper order
    HandleMiddlewareLoading();
}

void CoreEngine::Listen(const std::string& host, int port)
{
    connHandler_->Initialize(host, port);

    connHandler_->SetReceiveCallback([this](ConnectionContext* ctx){
        this->HandleRequest(ctx);
    });
    connHandler_->Run();
}

void CoreEngine::Stop()
{
    connHandler_->Stop();

    logger_.Info("[CoreEngine]: Stopped Successfully!");
}

// vvv Internals vvv
void CoreEngine::HandleRequest(ConnectionContext* ctx)
{
    // This will be transmitted through all the layers (from here to middleware to user)
    HttpResponse res;
    Response userRes{&res, WFX::Shared::GetHttpAPIV1()};

    HttpParseState state = HttpParser::Parse(ctx);
    auto& networkConfig  = config_.networkConfig;

    switch(state)
    {
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
            // Version is important for Serializer to properly create a response
            // HTTP/1.1 and HTTP/2 have different formats dawg
            res.version = ctx->requestInfo->version;

            auto& reqInfo    = *ctx->requestInfo;
            auto  connHeader = reqInfo.headers.GetHeader("Connection");
            
            bool shouldClose = (reqInfo.version == HttpVersion::HTTP_1_0)
                ? !StringSanitizer::CaseInsensitiveCompare(connHeader, "keep-alive") // HTTP/1.0: Defaults to close
                : StringSanitizer::CaseInsensitiveCompare(connHeader, "close");      // HTTP/1.1: Defaults to keep-alive
            
            res.Set("Connection", shouldClose ? "close" : "keep-alive");
            
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
                auto node = Router::GetInstance().MatchRoute(
                                    reqInfo.method,
                                    reqInfo.path,
                                    reqInfo.pathSegments
                                );

                if(!node)
                    res.Status(HttpStatus::NOT_FOUND).SendText("404: Route not found :(");
                else {
                    // Only execute user callback if middleware chain is successful
                    if(middleware_.ExecuteMiddleware(node, reqInfo, userRes))
                        (node->callback)(reqInfo, userRes);
                }
            }

            ctx->parseState = static_cast<std::uint8_t>(HttpParseState::PARSE_IDLE);
            connHandler_->RefreshExpiry(ctx, networkConfig.idleTimeout);

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

void CoreEngine::HandleResponse(HttpResponse& res, ConnectionContext* ctx, bool shouldClose)
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

// vvv HELPER STUFF vvv
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