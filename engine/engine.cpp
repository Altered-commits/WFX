#include "engine.hpp"

#include "include/http/response.hpp" // For 'Response'
#include "http/routing/router.hpp"
#include "shared/apis/master_api.hpp"

#include <iostream>
#include <string>
#include <thread>

namespace WFX::Core {

Engine::Engine()
    : connHandler_(CreateConnectionHandler())
{
    // Load stuff from wfx.toml if it exists, else we use default configuration
    config_.LoadFromFile("wfx.toml");

    // Load user's DLL file which we compiled above
    HandlerUserDLLInjection("api_entry.dll");
}

void Engine::Listen(const std::string& host, int port)
{
    if(!connHandler_->Initialize(host, port))
        logger_.Fatal("[Engine]: Failed to initialize server");

    logger_.Info("[Engine]: Listening on ", host, ':', port);

    connHandler_->Run([this](WFXSocket data) {
        this->HandleConnection(data);
    });
}

void Engine::Stop()
{
    connHandler_->Stop();

    logger_.Info("[Engine]: Stopped Successfully!");
}

// vvv Internals vvv
void Engine::HandleConnection(WFXSocket socket)
{
    connHandler_->SetReceiveCallback(socket, [this, socket](ConnectionContext& ctx) {
        this->HandleRequest(socket, ctx);
    });
}

void Engine::HandleRequest(WFXSocket socket, ConnectionContext& ctx)
{
    // This will be transmitted through all the layers (from here to middleware to user)
    HttpResponse res;
    Response userRes{&res, WFX::Shared::GetHttpAPIV1()};

    // Parse (The most obvious fucking comment i could write, i'm just sleepy rn cut me some slack)
    HttpParseState state = HttpParser::Parse(ctx);

    // Version is important for Serializer to properly create a response
    // HTTP/1.1 and HTTP/2 have different formats duh
    res.version = ctx.requestInfo->version;

    switch(state)
    {        
        case HttpParseState::PARSE_INCOMPLETE_HEADERS:
        case HttpParseState::PARSE_INCOMPLETE_BODY:
        {
            // Set the current tick to ensure timeout handler doesnt kill the context
            ctx.SetState(HttpConnectionState::ACTIVE);
            ctx.timeoutTick = connHandler_->GetCurrentTick();
            connHandler_->ResumeReceive(socket);
            return;
        }
        
        case HttpParseState::PARSE_EXPECT_100:
        {
            // We want to wait for the request so we won't be closing connection
            // also update timeoutTick so timeout handler doesnt kill the context
            ctx.SetState(HttpConnectionState::ACTIVE);
            ctx.timeoutTick = connHandler_->GetCurrentTick();
            connHandler_->Write(socket, "HTTP/1.1 100 Continue\r\n\r\n");
            return;
        }
        
        case HttpParseState::PARSE_EXPECT_417:
        {
            // Close the connection whether client wants to or not
            ctx.SetState(HttpConnectionState::CLOSING_DEFAULT);
            connHandler_->Write(socket, "HTTP/1.1 417 Expectation Failed\r\n\r\n");
            return;
        }

        case HttpParseState::PARSE_SUCCESS:
        {
            // So timeout handler doesn't do some weird stuff
            ctx.parseState  = static_cast<std::uint8_t>(HttpParseState::PARSE_DATA_OCCUPIED);
            
            // logger_.Info("[Engine]: ", ctx.connInfo.GetIpStr(), ':', socket, " on route '", ctx.requestInfo->path, "'");
            
            auto conn        = ctx.requestInfo->headers.GetHeader("Connection");
            bool shouldClose = true;

            if(!conn.empty()) {
                res.Set("Connection", std::string{conn});
                shouldClose = StringGuard::CaseInsensitiveCompare(conn, "close");
            }
            else
                res.Set("Connection", "close");

            // Get the callback for the route we got, if it doesn't exist, we display error
            auto callback = Router::GetInstance().MatchRoute(
                                ctx.requestInfo->method,
                                ctx.requestInfo->path,
                                ctx.requestInfo->pathSegments
                            );

            if(!callback)
                res.SendText("404: Route not found :(");
            else
                (*callback)(*ctx.requestInfo, userRes);

            // Set the 'connState' to whatever the status of Connection header is
            ctx.SetState(
                shouldClose
                    ? HttpConnectionState::CLOSING_DEFAULT
                    : HttpConnectionState::ACTIVE
                );
            ctx.parseState  = static_cast<std::uint8_t>(HttpParseState::PARSE_IDLE);
            ctx.timeoutTick = connHandler_->GetCurrentTick();

            HandleResponse(socket, res, ctx);
            return;
        }

        case HttpParseState::PARSE_ERROR:
        {   
            res.Status(HttpStatus::BAD_REQUEST)
                .Set("Connection", "close")
                .SendText("Bad Request");
                
            // Mark the connection to be closed after write completes
            ctx.SetState(HttpConnectionState::CLOSING_DEFAULT);
            HandleResponse(socket, res, ctx);
            return;
        }

        case HttpParseState::PARSE_STREAMING_BODY:
        default:
            logger_.Info("[Engine]: No Impl");
            res.Status(HttpStatus::NOT_IMPLEMENTED)
                .Set("Connection", "close")
                .SendText("Not Implemented");
            
            ctx.SetState(HttpConnectionState::CLOSING_DEFAULT);
            HandleResponse(socket, res, ctx);
            return;
    }
}

void Engine::HandleResponse(WFXSocket socket, HttpResponse& res, ConnectionContext& ctx)
{
    auto&& [serializedContent, bodyView] = HttpSerializer::Serialize(res);

    int writeStatus = 0;

    // File operation via TransmitFile / sendfile, res.body contains the file path
    if(res.IsFileOperation())
        writeStatus = connHandler_->WriteFile(socket, std::move(serializedContent), bodyView);
    // Regular WSASend write (text, JSON, etc)
    else
        writeStatus = connHandler_->Write(socket, serializedContent);

    // Status < 0 is an error, no GQCS event is generated in WorkerLoop, Close connections ourselves
    if(writeStatus < 0)
    {
        ctx.SetState(HttpConnectionState::CLOSING_IMMEDIATE);
        connHandler_->Close(socket);
        return;
    }

    // If the connection is marked for closure, we DO NOT touch the context here
    // The backend connection handler will handle the final cleanup after the write completes
    if(ctx.GetState() != HttpConnectionState::CLOSING_DEFAULT) {
        ctx.dataLength         = 0;
        ctx.expectedBodyLength = 0;
        ctx.trackBytes         = 0;
        // The state correctly remains ACTIVE
    }
}

// vvv USER DLL STUFF vvv
void Engine::HandlerUserDLLInjection(const char* path)
{
    HMODULE userModule = LoadLibraryA(path);
    if(!userModule)
        logger_.Fatal("[Engine]: User side 'api_entry.dll' not found.");

    // Resolve the exported function
    auto registerFn = reinterpret_cast<WFX::Shared::RegisterMasterAPIFn>(
        GetProcAddress(userModule, "RegisterMasterAPI")
    );

    if(!registerFn)
        logger_.Fatal("[Engine]: Failed to find RegisterWFXAPI() in user DLL.");
    
    // Inject API
    registerFn(WFX::Shared::GetMasterAPI());

    logger_.Info("[Engine]: Successfully injected API and initialized user module.");
}

} // namespace WFX