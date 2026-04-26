#ifndef WFX_HTTP_REQUEST_HPP
#define WFX_HTTP_REQUEST_HPP

#include "http/headers/http_headers.hpp"
#include "http/routing/route_segment.hpp"
#include "shared/abis/constants.hpp"
#include "shared/abis/any.hpp"

#include <string>

// Forward declare engine to access cool internal stuff
namespace WFX::Core { class CoreEngine; }

// Just defines the structure of request
namespace WFX::Http {

// Context storage for middleware / routes / user stuff
using ContextMap = std::unordered_map<std::string, Shared::Any>;

struct HttpRequest {
    Shared::HttpMethod  method;
    Shared::HttpVersion version;
    std::string_view    path;
    std::string_view    body;
    RequestHeaders      headers;
    ContextMap          context;
    PathSegments        pathSegments;

public: // Copying is strictly not allowed
    HttpRequest(const HttpRequest&)            = delete;
    HttpRequest& operator=(const HttpRequest&) = delete;

    HttpRequest() = default;

public: // Helper functions
    void ClearInfo()
    {
        for(auto& [k, v] : context)
            v.Reset();

        routeNode_ = nullptr;
        headers.Clear();
        pathSegments.clear(); 
        context.clear();
    }

private:
    const void* routeNode_ = nullptr;

    friend class Core::CoreEngine;
};

} // namespace WFX::Http

#endif //WFX_HTTP_REQUEST_HPP