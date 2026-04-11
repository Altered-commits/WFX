#ifndef WFX_HTTP_MIDDLEWARE_HPP
#define WFX_HTTP_MIDDLEWARE_HPP

#include "shared/abis/types.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace WFX::Http {

// Fwd declare stuff
struct TrieNode;
struct ConnectionContext;

using MiddlewareStack       = std::vector<Shared::MwCallback>;
using MiddlewareConfigOrder = const std::vector<std::string>&;
using MiddlewareFactory     = std::unordered_map<std::string_view, Shared::MwCallback>;
using MiddlewarePerRoute    = std::unordered_map<const TrieNode*, MiddlewareStack>;

struct MiddlewareResult {
    bool success;
    bool isAsync;  // true = engine should wait for callback
};

struct MiddlewareFunctionResult {
    Shared::MiddlewareAction action;
    bool isAsync;
};

class HttpMiddleware {
public:
    HttpMiddleware()  = default;
    ~HttpMiddleware() = default;

public:
    void RegisterMiddleware(std::string_view name, Shared::MwCallback mw);
    void RegisterPerRouteMiddleware(const TrieNode* node, MiddlewareStack mwStack);

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
        ConnectionContext* ctx, Request req, Response res, MiddlewareStack& stack
    );
    MiddlewareFunctionResult ExecuteFunction(
        ConnectionContext* ctx, Request req, Response res, Shared::MwCallback mw
    );

private:
    // Temporary construct
    MiddlewareFactory middlewareFactories_;

    // Main stuff
    MiddlewareStack     middlewareGlobalCallbacks_;
    MiddlewarePerRoute  middlewarePerRouteCallbacks_;
};

} // namespace WFX::Http

#endif // WFX_HTTP_MIDDLEWARE_HPP