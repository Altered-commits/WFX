#ifndef WFX_CORE_ENGINE_HPP
#define WFX_CORE_ENGINE_HPP

#include "config/config.hpp"
#include "http/connection/http_connection_factory.hpp"
#include "http/middleware/http_middleware.hpp"
#include "http/routing/router.hpp"

#include <string>

namespace WFX::Core {

class CoreEngine {
public: // Main Stuff
    CoreEngine(const char* dllPath, bool useHttps);
    void Listen(const std::string& host, std::uint16_t port);
    void Stop();

public: // Static stuff
    static void OnCoroutineComplete(void* ud, Shared::AsyncResult result);

private: // Internal Functions
    void HandleRequest(Http::ConnectionContext* ctx);
    void HandleResponse(Http::ConnectionContext* ctx);
    void HandleSuccess(Http::ConnectionContext* ctx);

private: // Helper Functions
    void         FinishRequest(Http::ConnectionContext* ctx);
    void         HandleError(Http::ConnectionContext* ctx, Shared::HttpStatus code, std::string_view message);
    std::uint8_t HandleConnectionHeader(std::string_view header);
    void         HandleUserDLLInjection(const char* dllDir);
    void         HandleMiddlewareLoading();

private:
    Config&        config_ = Config::GetInstance();
    Utils::Logger& logger_ = Utils::Logger::GetInstance();
    
    Http::HttpMiddleware middleware_;
    Http::Router         router_;

    std::unique_ptr<Http::HttpConnectionHandler> connHandler_;
};

} // namespace WFX

#endif // WFX_CORE_ENGINE_HPP