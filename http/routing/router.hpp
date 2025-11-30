#ifndef WFX_HTTP_ROUTER_HPP
#define WFX_HTTP_ROUTER_HPP

#include "route_trie.hpp"

#include "http/constants/http_constants.hpp"

namespace WFX::Http {

class Router {
public:
    Router()  = default;
    ~Router() = default;

public:
    const TrieNode* RegisterRoute(HttpMethod method, std::string_view path, HttpCallbackType handler);
    const TrieNode* MatchRoute(HttpMethod method, std::string_view path, PathSegments& outParams) const;

    void PushRouteGroup(std::string_view prefix);
    void PopRouteGroup();

private:
    RouteTrie getRoutes_;
    RouteTrie postRoutes_;

    // No need for copy and move constructors
    Router(const Router&)            = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&)                 = delete;
    Router& operator=(Router&&)      = delete;
};

} // namespace WFX::Http

#endif // WFX_HTTP_ROUTER_HPP