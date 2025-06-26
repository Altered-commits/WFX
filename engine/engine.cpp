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
        this->HandleConnection(std::move(data));
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
    HttpRequest req;

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

        connHandler_->Write(socket, badResp, std::strlen(badResp));

        // You may or may not want to resume receive, depending on keep-alive support
        connHandler_->Close(socket); // Close since it's a bad request
        return;
    }

    // === Parsing succeeded, handle normally ===

    // Example: check method and path
    if(req.method == HttpMethod::GET && req.path == "/hello")
    {
        const char* httpResp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: keep-alive\r\n"
            "Content-Length: 14\r\n"
            "\r\n"
            "Hello from WFX";

        connHandler_->Write(socket, httpResp, 104);
    }
    else
    {
        const char* notFound =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Connection: keep-alive\r\n"
            "Content-Length: 9\r\n"
            "\r\n"
            "Not Found";

        connHandler_->Write(socket, notFound, 105);
    }

    // === Cleanup + resume ===
    ctx.dataLength = 0;                    // Clear current buffer
    connHandler_->ResumeReceive(socket);   // Re-arm socket for next request
}

} // namespace WFX