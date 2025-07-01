#include "engine.hpp"

#include <iostream>
#include <string>
#include <thread>

namespace WFX::Core {

Engine::Engine()
    : connHandler_(CreateConnectionHandler())
{}

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
        logger_.Info("Connected IP Address: ", ctx.connInfo.GetIpStr(),
            " of type: ", ctx.connInfo.GetIpType());

        this->HandleRequest(socket, ctx);
    });
}

void Engine::HandleRequest(WFXSocket socket, ConnectionContext& ctx)
{
    // This will be transmitted through all the layers (from here to middleware to user)
    HttpResponse res;
    HttpParseState state = HttpParser::Parse(ctx);

    logger_.Info("Current State of Parser: ", static_cast<std::uint64_t>(state), " on Socket: ", socket, " with Length: ", ctx.dataLength);

    // Respond with 400 Bad Request if parsing failed
    if(state == HttpParseState::PARSE_ERROR) {
        const char* badResp =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n"
            "Content-Length: 11\r\n"
            "\r\n"
            "Bad Request";

        connHandler_->Write(socket, badResp);

        // Mark the connection to be closed after write completes
        ctx.parseInfo.shouldClose = true;
        return;
    }

    // Need more stuff, resume receive
    if(
        state == HttpParseState::PARSE_INCOMPLETE_BODY ||
        state == HttpParseState::PARSE_INCOMPLETE_HEADERS
    ) {
        connHandler_->ResumeReceive(socket);
        return;
    }

    // Response stage
    res.version = ctx.requestInfo->version;

    res.Status(HttpStatus::OK)
        .Set("X-Powered-By", "WFX")
        .Set("Server", "WFX/1.0")
        .Set("Cache-Control", "no-store")
        .Set("Connection", "keep-alive")
        .SendFile("test.html");

    HandleResponse(socket, res, ctx);
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
    ctx.parseInfo  = { 0 };
    ctx.dataLength = 0;
}

} // namespace WFX