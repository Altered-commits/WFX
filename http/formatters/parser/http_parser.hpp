#ifndef WFX_HTTP_PARSER_HPP
#define WFX_HTTP_PARSER_HPP

#include "http/headers/http_headers.hpp"
#include "http/constants/http_constants.hpp"
#include "http/connection/http_connection.hpp"
#include "http/request/http_request.hpp"

#include "utils/backport/string.hpp"

#include <memory>
#include <string>
#include <vector>
#include <charconv>

namespace WFX::Http {

enum class HttpParseState {
    PARSE_INCOMPLETE_HEADERS, // Header end sequence (\r\n\r\n) not found yet
    PARSE_INCOMPLETE_BODY,    // Buffering body (Content-Length not fully received)
    
    PARSE_STREAMING_BODY,     // [Future] Streaming mode (body being processed in chunks)

    PARSE_SUCCESS,            // Successfully received and parsed all data
    PARSE_ERROR               // Malformed request
};

// Being used as a namespace rn, fun
class HttpParser {
public:
    static HttpParseState Parse(ConnectionContext& ctx);

private: // Parse helpers
    static bool ParseRequest(const char* data, std::size_t size, std::size_t& pos, HttpRequest& outRequest);
    static bool ParseHeaders(const char* data, std::size_t size, std::size_t& pos, RequestHeaders& outHeaders);
    static bool ParseBody(const char* data, std::size_t size, std::size_t& pos, std::size_t contentLen, HttpRequest& outRequest);

private: // Helpers
    static bool SafeFindCRLF(const char* data, std::size_t size, std::size_t from, std::size_t& outNextPos, std::string_view& outLine);
    static bool SafeFindHeaderEnd(const char* data, std::size_t size, std::size_t from, std::size_t& outPos);
    static std::string_view Trim(std::string_view sv);

private: // Limits
    static constexpr size_t MAX_HEADER_TOTAL_SIZE   = 8192;
    static constexpr size_t MAX_HEADERS_TOTAL_COUNT = 64;
    static constexpr size_t MAX_BODY_TOTAL_SIZE     = 8192;
};

} // namespace WFX::Http

#endif // WFX_HTTP_PARSER_HPP