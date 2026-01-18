#ifndef WFX_HTTP_PARSER_HPP
#define WFX_HTTP_PARSER_HPP

#include "http/connection/http_connection.hpp"

namespace WFX::Http {

namespace HttpParser {
    HttpParseState Parse(ConnectionContext* ctx);
} // namespace HttpParser

} // namespace WFX::Http

#endif // WFX_HTTP_PARSER_HPP