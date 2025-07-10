#include "engine.hpp"

#include <iostream>
#include <string>
#include <thread>

namespace WFX::Core {

Engine::Engine()
    : connHandler_(CreateConnectionHandler())
{
    // Load stuff from wfx.toml if it exists, else we use default configuration
    config_.LoadFromFile("wfx.toml");
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
        logger_.Info("[Engine]: Request on IP: ", ctx.connInfo.GetIpStr(), ':', socket);

        this->HandleRequest(socket, ctx);
    });
}

void Engine::HandleRequest(WFXSocket socket, ConnectionContext& ctx)
{
    // This will be transmitted through all the layers (from here to middleware to user)
    HttpResponse res;
    HttpParseState state = HttpParser::Parse(ctx);

    switch(state)
    {
        case HttpParseState::PARSE_ERROR:
        {
            const char* badResp =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain\r\n"
                "Connection: close\r\n"
                "Content-Length: 11\r\n"
                "\r\n"
                "Bad Request";

            connHandler_->Write(socket, badResp);

            // Mark the connection to be closed after write completes
            ctx.shouldClose = true;
            return;
        }
        
        case HttpParseState::PARSE_INCOMPLETE_HEADERS:
        case HttpParseState::PARSE_INCOMPLETE_BODY:
        {
            // Set the current tick to ensure timeout handler doesnt kill the context
            ctx.timeoutTick = connHandler_->GetCurrentTick();
            connHandler_->ResumeReceive(socket);
            return;
        }
        
        case HttpParseState::PARSE_EXPECT_100:
        {
            // We want to wait for the request so we won't be closing connection
            // also update timeoutTick so timeout handler doesnt kill the context
            ctx.shouldClose = false;
            ctx.timeoutTick = connHandler_->GetCurrentTick();
            connHandler_->Write(socket, "HTTP/1.1 100 Continue\r\n\r\n");
            return;
        }
        
        case HttpParseState::PARSE_EXPECT_417:
        {
            // Close the connection whether client wants to or not
            ctx.shouldClose = true;
            connHandler_->Write(socket, "HTTP/1.1 417 Expectation Failed\r\n\r\n");
            return;
        }

        case HttpParseState::PARSE_SUCCESS:
        {
            // Response stage
            res.version = ctx.requestInfo->version;

            if(ctx.shouldClose)
                res.Set("Connection", "close");
            else {
                res.Set("Connection", "keep-alive");
                // Because we want the connection to be alive, switch the state to being idle
                // Also the timeoutTick, DO NOT FORGET
                ctx.parseState  = static_cast<std::uint8_t>(HttpParseState::PARSE_IDLE);
                ctx.timeoutTick = connHandler_->GetCurrentTick();
            }

            res.Status(HttpStatus::OK)
                .Set("X-Powered-By", "WFX")
                .Set("Server", "WFX/1.0")
                .Set("Cache-Control", "no-store")
                .SendFile("test.html");

            HandleResponse(socket, res, ctx);
            break;
        }

        case HttpParseState::PARSE_STREAMING_BODY:
        default:
            logger_.Info("[Engine]: No Impl");
            res.Status(HttpStatus::NOT_IMPLEMENTED)
                .Set("Connection", "close")
                .SendText("Not Implemented");
            
            ctx.shouldClose = true;
            HandleResponse(socket, res, ctx);
            break;
    }
}

void Engine::HandleResponse(WFXSocket socket, HttpResponse& res, ConnectionContext& ctx)
{   
    std::string serializedContent = HttpSerializer::Serialize(res);
    
    // File operation via TransmitFile, res.body contains the file path
    if(res.IsFileOperation())
        connHandler_->WriteFile(socket, std::move(serializedContent), res.body);
    // Regular WSASend write (text, JSON, etc)
    else
        connHandler_->Write(socket, serializedContent);

    // vvv Cleanup vvv
    ctx.shouldClose = 0;
    ctx.trackBytes  = 0;
    ctx.dataLength  = 0;
    ctx.expectedBodyLength = 0;
}

} // namespace WFX