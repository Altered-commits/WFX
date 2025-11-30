#ifndef WFX_CORE_ENGINE_HPP
#define WFX_CORE_ENGINE_HPP

#include "config/config.hpp"
#include "http/connection/http_connection_factory.hpp"
#include "http/middleware/http_middleware.hpp"
#include "http/routing/router.hpp"

#include <string>

namespace WFX::Core {

using namespace WFX::Utils; // For 'Logger', 'FileSystem', 'ProcessUtils'
using namespace WFX::Http;  // For 'HttpConnectionHandler', 'HttpParser', 'HttpRequest', 'HttpMiddleware'

class CoreEngine {
public: // Main Stuff
    CoreEngine(const char* dllPath, bool useHttps);
    void Listen(const std::string& host, int port);
    void Stop();

private: // Internal Functions
    void HandleRequest(ConnectionContext* ctx);
    void HandleResponse(ConnectionContext* ctx);

private: // Helper Functions
    std::uint8_t HandleConnectionHeader(std::string_view header);
    void         HandleUserDLLInjection(const char* dllDir);
    void         HandleMiddlewareLoading();

private:
    Logger& logger_ = Logger::GetInstance();
    Config& config_ = Config::GetInstance();
    
    HttpMiddleware middleware_;
    Router         router_;

    std::unique_ptr<HttpConnectionHandler> connHandler_;
};

} // namespace WFX

#endif // WFX_CORE_ENGINE_HPP