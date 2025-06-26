#ifndef WFX_HTTP_REQUEST_HPP
#define WFX_HTTP_REQUEST_HPP

#include "http/constants/http_constants.hpp"
#include "http/headers/http_headers.hpp"

#include <string>

// Just defines the structure of request
namespace WFX::Http {

struct HttpRequest {
    HttpMethod       method;
    HttpVersion      version;
    HttpHeaders      headers;
    std::string_view path;
    std::string_view body;
};

} // namespace WFX::Http


#endif //WFX_HTTP_REQUEST_HPP