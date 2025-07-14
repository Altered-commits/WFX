#ifndef WFX_HTTP_ROUTER_HPP
#define WFX_HTTP_ROUTER_HPP

#include "route_trie.hpp"

#include "include/export_signature.hpp"
#include "http/constants/http_constants.hpp"

namespace WFX::Http {

class WFX_API Router {
public:
    static Router& GetInstance();

    void                    RegisterRoute(HttpMethod method, std::string_view path, HttpCallbackType handler);
    const HttpCallbackType* MatchRoute(HttpMethod method, std::string_view path, PathSegments& outParams) const;

private:
    RouteTrie getRoutes_;
    RouteTrie postRoutes_;

    Router()  = default;
    ~Router() = default;

    // No need for copy and move constructors
    Router(const Router&)            = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&)                 = delete;
    Router& operator=(Router&&)      = delete;
};

} // namespace WFX::Http

#endif // WFX_HTTP_ROUTER_HPP