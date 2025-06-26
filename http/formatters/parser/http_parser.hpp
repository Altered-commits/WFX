#ifndef WFX_HTTP_PARSER_HPP
#define WFX_HTTP_PARSER_HPP

#include "http/headers/http_headers.hpp"
#include "http/constants/http_constants.hpp"
#include "http/connection/http_connection.hpp"
#include "http/request/http_request.hpp"

#include <memory>
#include <string>
#include <vector>
#include <charconv>

namespace WFX::Http {

// Being used as a namespace rn, fun
class HttpParser {
public:
    static bool Parse(ConnectionContext& ctx, HttpRequest& outRequest);

private: // Parse helpers
    static bool        ParseRequest(const char* data, std::size_t size, std::size_t& pos, HttpRequest& outRequest);
    static bool        ParseHeaders(const char* data, std::size_t size, std::size_t& pos, HttpHeaders& outHeaders);
    static bool        ParseBody(const char* data, std::size_t size, std::size_t& pos, HttpRequest& outRequest);
    static HttpMethod  ParseHttpMethod(std::string_view method);
    static HttpVersion ParseHttpVersion(std::string_view version);

private: // Helpers
    static bool SafeFindCRLF(const char* data, std::size_t size, std::size_t from, std::size_t& outNextPos, std::string_view& outLine);
    static std::string_view Trim(std::string_view sv);

private: // Limits
    static constexpr size_t MAX_HEADER_TOTAL_SIZE   = 8192;
    static constexpr size_t MAX_HEADERS_TOTAL_COUNT = 64;
    static constexpr size_t MAX_BODY_TOTAL_SIZE     = 8192;
};

} // namespace WFX::Http

#endif // WFX_HTTP_PARSER_HPP