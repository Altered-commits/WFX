#ifndef WFX_HTTP_RESPONSE_HPP
#define WFX_HTTP_RESPONSE_HPP

#include "http/constants/http_constants.hpp"
#include "http/headers/http_headers.hpp"

#include "third_party/nlohmann/json.hpp"

#include <string>

// To keep naming consistent :)
using Json = nlohmann::json;

namespace WFX::Http {

struct HttpResponse {
    HttpVersion version = HttpVersion::HTTP_1_1;
    HttpStatus  status  = HttpStatus::OK;
    HttpHeaders headers;
    std::string body;

    // Setters
    HttpResponse& Status(HttpStatus code);
    HttpResponse& Set(std::string_view key, std::string_view value);
    void          Body(std::string_view body);

    // Senders
    void SendText(std::string_view text);
    void SendJson(const Json& json);
    void SendHtml(std::string_view path);
};

} // namespace WFX::Http


#endif // WFX_HTTP_RESPONSE_HPP