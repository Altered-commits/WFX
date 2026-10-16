// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_ERROR_MESSAGES_HPP
#define WFX_HTTP_ERROR_MESSAGES_HPP

namespace WFX::Http {
namespace HttpError {

// These are whole responses, written to the socket as they stand: they are what the engine has to
// say when a request failed before there was a Response object to say it through. The body length
// is spelled out beside the body, so the macro asserts the two agree. Counted short, a client
// waits on bytes that never arrive; counted long, it reads the next response as part of this one
// NOLINTBEGIN(bugprone-macro-parentheses) 'name' is the thing being declared, it cannot be bracketed
#define WFX_HTTP_ERROR(name, statusLine, body, length)                                                                 \
    static_assert(sizeof(body) - 1 == (length), "Content-Length does not match the body of " #name);                   \
    static constexpr const char* name = "HTTP/1.1 " statusLine "\r\n"                                                  \
                                        "Content-Type: text/plain; charset=UTF-8\r\n"                                  \
                                        "Connection: close\r\n"                                                        \
                                        "Content-Length: " #length "\r\n"                                              \
                                        "\r\n" body
// NOLINTEND(bugprone-macro-parentheses)

WFX_HTTP_ERROR(BAD_REQUEST, "400 Bad Request", "Bad Request", 11);
WFX_HTTP_ERROR(FORBIDDEN, "403 Forbidden", "Forbidden", 9);
WFX_HTTP_ERROR(NOT_FOUND, "404 Not Found", "Not Found", 9);
WFX_HTTP_ERROR(METHOD_NOT_ALLOWED, "405 Method Not Allowed", "Method Not Allowed", 18);
WFX_HTTP_ERROR(PAYLOAD_TOO_LARGE, "413 Payload Too Large", "Payload Too Large", 17);
WFX_HTTP_ERROR(URI_TOO_LONG, "414 URI Too Long", "URI Too Long", 12);
WFX_HTTP_ERROR(INTERNAL_ERROR, "500 Internal Server Error", "Internal Server Error", 21);
WFX_HTTP_ERROR(NOT_IMPLEMENTED, "501 Not Implemented", "Not Implemented", 15);
WFX_HTTP_ERROR(SERVICE_UNAVAILABLE, "503 Service Unavailable", "Service Unavailable", 19);

#undef WFX_HTTP_ERROR

} // namespace HttpError
} // namespace WFX::Http

#endif // WFX_HTTP_ERROR_MESSAGES_HPP
