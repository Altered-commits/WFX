#ifndef WFX_ENGINE_HPP
#define WFX_ENGINE_HPP

#include "config/config.hpp"
#include "http/connection/http_connection_factory.hpp"

#include <string>

namespace WFX::Core {

using namespace WFX::Utils; // For 'Logger'
using namespace WFX::Http;  // For 'HttpConnectionHandler', 'HttpParser', 'HttpRequest'

class Engine {
public:
    Engine();
    void Listen(const std::string& host, int port);
    void Stop();

private:
    void HandleConnection(WFXSocket client);
    void HandleRequest(WFXSocket socket, ConnectionContext& ctx);
    void HandleResponse(WFXSocket socket, HttpResponse& res, ConnectionContext& ctx);

private:
    void HandleUserSrcCompilation(const char* dllDir, const char* dllPath);
    void HandleUserDLLInjection(const char* dllDir);

    Logger& logger_ = Logger::GetInstance();
    Config& config_ = Config::GetInstance();

    std::unique_ptr<HttpConnectionHandler> connHandler_;
};

} // namespace WFX

#endif // WFX_ENGINE_HPP