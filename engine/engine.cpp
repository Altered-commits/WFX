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
    // These both will be transmitted through all the layers (from here to middleware to user)
    HttpRequest  req;
    HttpResponse res;

    if(!HttpParser::Parse(ctx, req))
    {
        // Respond with 400 Bad Request if parsing failed
        const char* badResp =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: close\r\n"
            "Content-Length: 11\r\n"
            "\r\n"
            "Bad Request";

        connHandler_->Write(socket, badResp, 105);

        // You may or may not want to resume receive, depending on keep-alive support
        connHandler_->Close(socket); // Close since it's a bad request
        return;
    }

    // JSON response
    Json obj = {
        {"ip_addr", ctx.connInfo.GetIpStr()},
        {"ip_type", ctx.connInfo.GetIpType()},
        {"length", ctx.dataLength},
        {"size", ctx.bufferSize}
    };

    // Response stage
    res.version = req.version;

    res.Status(HttpStatus::OK)
        .Set("X-Powered-By", "WFX")
        .Set("Server", "WFX/1.0")
        .Set("Cache-Control", "no-store")
        .Set("Connection", "keep-alive")
        .SendJson(obj);

    // Serializer stage
    std::string writableString = HttpSerializer::Serialize(res);
    connHandler_->Write(socket, writableString.data(), writableString.size());
    
    // vvv Cleanup + resume vvv
    ctx.dataLength = 0;                    // Clear current buffer
    connHandler_->ResumeReceive(socket);   // Re-arm socket for next request
}

} // namespace WFX