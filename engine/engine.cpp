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
    // Hardcode the request for now, later we do the do
    std::string httpResp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: keep-alive\r\n"
        "Content-Length: 14\r\n"
        "\r\n"
        "Hello from WFX";

    connHandler_->Write(socket, httpResp.c_str(), httpResp.size());

    // Super important, without this, shit breaks
    connHandler_->ResumeReceive(socket);
}

} // namespace WFX