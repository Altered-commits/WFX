#ifndef WFX_HTTP_MIDDLEWARE_HPP
#define WFX_HTTP_MIDDLEWARE_HPP

#include "shared/http/common.hpp"
#include <unordered_map>

namespace WFX::Http {

// Fwd declare stuff
struct TrieNode;
struct ConnectionContext;

using namespace WFX::Shared;

using MiddlewareName        = std::string_view;
using MiddlewareConfigOrder = const std::vector<std::string>&;
using MiddlewareFactory     = std::unordered_map<MiddlewareName, HttpMiddlewareType>;
using MiddlewarePerRoute    = std::unordered_map<const TrieNode*, HttpMiddlewareStack>;

// 1st parameter is whether we successfully executed all middleware or no
// 2nd parameter is for async functionality
using MiddlewareResult         = std::pair<bool, AsyncMiddlewareAction>;
using MiddlewareFunctionResult = std::pair<MiddlewareAction, AsyncMiddlewareAction>;

class HttpMiddleware {
public:
    HttpMiddleware()  = default;
    ~HttpMiddleware() = default;

public:
    void RegisterMiddleware(MiddlewareName name, HttpMiddlewareType mw);
    void RegisterPerRouteMiddleware(const TrieNode* node, HttpMiddlewareStack mwStack);

    MiddlewareResult ExecuteMiddleware(
        ConnectionContext* ctx, const TrieNode* node, Request req, Response res
    );

    // Using std::string because TOML loader returns vector<string>
    void LoadMiddlewareFromConfig(MiddlewareConfigOrder order);

    void DiscardFactoryMap();

private:
    HttpMiddleware(const HttpMiddleware&)            = delete;
    HttpMiddleware& operator=(const HttpMiddleware&) = delete;

private: // Helper functions
    MiddlewareResult ExecuteHelper(
        ConnectionContext* ctx, HttpMiddlewareStack& stack, Request req, Response res
    );
    MiddlewareFunctionResult ExecuteFunction(
        ConnectionContext* ctx, HttpMiddlewareType& entry, Request req, Response res
    );

private:
    // Temporary construct
    MiddlewareFactory middlewareFactories_;

    // Main stuff
    HttpMiddlewareStack middlewareGlobalCallbacks_;
    MiddlewarePerRoute  middlewarePerRouteCallbacks_;
};

} // namespace WFX::Http

#endif // WFX_HTTP_MIDDLEWARE_HPP