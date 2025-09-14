#ifndef WFX_HTTP_PARSER_HPP
#define WFX_HTTP_PARSER_HPP

#include "http/headers/http_headers.hpp"
#include "http/constants/http_constants.hpp"
#include "http/connection/http_connection.hpp"
#include "http/request/http_request.hpp"

#include <string_view>

namespace WFX::Http {

// Being used as a namespace rn, fun
class HttpParser final {
public:
    static HttpParseState Parse(ConnectionContext* ctx);

private: // Parse helpers
    static bool ParseRequest(const char* data, std::size_t size, std::size_t& pos, HttpRequest& outRequest);
    static bool ParseHeaders(const char* data, std::size_t size, std::size_t& pos, RequestHeaders& outHeaders);
    static bool ParseBody(const char* data, std::size_t size, std::size_t& pos, std::size_t contentLen, HttpRequest& outRequest);

private: // Helpers
    static bool SafeFindCRLF(const char* data, std::size_t size, std::size_t from, std::size_t& outNextPos, std::string_view& outLine);
    static bool SafeFindHeaderEnd(const char* data, std::size_t size, std::size_t from, std::size_t& outPos);
    static std::string_view Trim(std::string_view sv);

private:
    HttpParser()  = delete;
    ~HttpParser() = delete;
};

} // namespace WFX::Http

#endif // WFX_HTTP_PARSER_HPP