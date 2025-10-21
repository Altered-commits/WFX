#ifndef WFX_HTTP_MIDDLEWARE_HPP
#define WFX_HTTP_MIDDLEWARE_HPP

#include "http/common/http_route_common.hpp"
#include <unordered_map>

namespace WFX::Http {

// Forward declare TrieNode*, defined inside of routing/route_segment.hpp
struct TrieNode;

using MiddlewareName        = std::string_view;
using MiddlewareConfigOrder = const std::vector<std::string>&;
using MiddlewareFactory     = std::unordered_map<MiddlewareName, MiddlewareCallbackType>;
using MiddlewarePerRoute    = std::unordered_map<const TrieNode*, MiddlewareStack>;

class HttpMiddleware {
public:
    static HttpMiddleware& GetInstance();

    void RegisterMiddleware(MiddlewareName name, MiddlewareCallbackType mw);
    void RegisterPerRouteMiddleware(const TrieNode* node, MiddlewareStack mwStack);
    bool ExecuteMiddleware(const TrieNode* node, HttpRequest& req, Response& res);
    
    // Using std::string because TOML loader returns vector<string>
    void LoadMiddlewareFromConfig(MiddlewareConfigOrder order);

    void DiscardFactoryMap();

private:
    HttpMiddleware()  = default;
    ~HttpMiddleware() = default;

    HttpMiddleware(const HttpMiddleware&)            = delete;
    HttpMiddleware& operator=(const HttpMiddleware&) = delete;
    HttpMiddleware(HttpMiddleware&&)                 = delete;
    HttpMiddleware& operator=(HttpMiddleware&&)      = delete;

private: // Helper functions
    bool ExecuteHelper(HttpRequest& req, Response& res, const MiddlewareStack& stack);

private:
    MiddlewareFactory  middlewareFactories_;
    MiddlewareStack    middlewareGlobalCallbacks_;
    MiddlewarePerRoute middlewarePerRouteCallbacks_;
};

} // namespace WFX::Http

#endif // WFX_HTTP_MIDDLEWARE_HPP