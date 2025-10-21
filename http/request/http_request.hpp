#ifndef WFX_HTTP_REQUEST_HPP
#define WFX_HTTP_REQUEST_HPP

#include "http/constants/http_constants.hpp"
#include "http/headers/http_headers.hpp"
#include "http/common/http_route_common.hpp"

#include <string>
#include <any>

// Just defines the structure of request
namespace WFX::Http {

// Context storage for middleware / routes / user stuff
using ContextMap = std::unordered_map<std::string, std::any>;

struct HttpRequest {
    HttpMethod       method;
    HttpVersion      version;
    RequestHeaders   headers;
    ContextMap       context;
    PathSegments     pathSegments;
    std::string_view path;

public: // Copying is strictly not allowed
    HttpRequest(const HttpRequest&)            = delete;
    HttpRequest& operator=(const HttpRequest&) = delete;

public: // Helper functions
    void ClearInfo()
    {
        headers.Clear();
        // pathSegments.clear(); NOTE: Cleared by WFX::Http::Router by default 
        context.clear();
    }

    template<typename T>
    void SetContext(const std::string& key, T&& value)
    {
        context[key] = std::forward<T>(value);
    }

    template <typename T>
    const T* GetContext(const std::string& key) const
    {
        auto it = context.find(key);
        if(it == context.end())
            return nullptr;
        
        return std::any_cast<const T>(&(it->second));
    }
};

} // namespace WFX::Http

#endif //WFX_HTTP_REQUEST_HPP