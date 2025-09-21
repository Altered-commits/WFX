#ifndef WFX_HTTP_MIDDLEWARE_HPP
#define WFX_HTTP_MIDDLEWARE_HPP

#include "http/common/route_common.hpp"
#include <unordered_map>

namespace WFX::Http {

using MiddlewareName    = std::string_view;
using MiddlewareStack   = std::vector<MiddlewareCallbackType>;
using MiddlewareOrder   = const std::vector<std::string>&;
using MiddlewareFactory = std::unordered_map<MiddlewareName, MiddlewareCallbackType>;

class HttpMiddleware {
public:
    static HttpMiddleware& GetInstance();

    void RegisterMiddleware(MiddlewareName name, MiddlewareCallbackType mw);
    bool ExecuteMiddleware(HttpRequest& req, Response& res);
    
    // Using std::string because TOML loader returns vector<string>
    void LoadMiddlewareFromConfig(MiddlewareOrder order);

    void DiscardFactoryMap();

private:
    HttpMiddleware()  = default;
    ~HttpMiddleware() = default;

    HttpMiddleware(const HttpMiddleware&)            = delete;
    HttpMiddleware& operator=(const HttpMiddleware&) = delete;
    HttpMiddleware(HttpMiddleware&&)                 = delete;
    HttpMiddleware& operator=(HttpMiddleware&&)      = delete;

private:
    MiddlewareFactory middlewareFactories_;
    MiddlewareStack   middlewareCallbacks_;
};

} // namespace WFX::Http

#endif // WFX_HTTP_MIDDLEWARE_HPP