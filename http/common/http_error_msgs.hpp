// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_HTTP_ERROR_MESSAGES_HPP
#define WFX_HTTP_ERROR_MESSAGES_HPP

namespace WFX::Http {
namespace HttpError {

// 400 Bad Request
static constexpr const char* BAD_REQUEST = "HTTP/1.1 400 Bad Request\r\n"
                                           "Content-Type: text/plain; charset=UTF-8\r\n"
                                           "Connection: close\r\n"
                                           "Content-Length: 12\r\n"
                                           "\r\n"
                                           "Bad Request";

// 403 Forbidden
static constexpr const char* FORBIDDEN = "HTTP/1.1 403 Forbidden\r\n"
                                         "Content-Type: text/plain; charset=UTF-8\r\n"
                                         "Connection: close\r\n"
                                         "Content-Length: 9\r\n"
                                         "\r\n"
                                         "Forbidden";

// 404 Not Found
static constexpr const char* NOT_FOUND = "HTTP/1.1 404 Not Found\r\n"
                                         "Content-Type: text/plain; charset=UTF-8\r\n"
                                         "Connection: close\r\n"
                                         "Content-Length: 9\r\n"
                                         "\r\n"
                                         "Not Found";

// 405 Method Not Allowed
static constexpr const char* METHOD_NOT_ALLOWED = "HTTP/1.1 405 Method Not Allowed\r\n"
                                                  "Content-Type: text/plain; charset=UTF-8\r\n"
                                                  "Connection: close\r\n"
                                                  "Content-Length: 18\r\n"
                                                  "\r\n"
                                                  "Method Not Allowed";

// 413 Payload Too Large
static constexpr const char* PAYLOAD_TOO_LARGE = "HTTP/1.1 413 Payload Too Large\r\n"
                                                 "Content-Type: text/plain; charset=UTF-8\r\n"
                                                 "Connection: close\r\n"
                                                 "Content-Length: 19\r\n"
                                                 "\r\n"
                                                 "Payload Too Large";

// 414 URI Too Long
static constexpr const char* URI_TOO_LONG = "HTTP/1.1 414 URI Too Long\r\n"
                                            "Content-Type: text/plain; charset=UTF-8\r\n"
                                            "Connection: close\r\n"
                                            "Content-Length: 13\r\n"
                                            "\r\n"
                                            "URI Too Long";

// 500 Internal Server Error
static constexpr const char* INTERNAL_ERROR = "HTTP/1.1 500 Internal Server Error\r\n"
                                              "Content-Type: text/plain; charset=UTF-8\r\n"
                                              "Connection: close\r\n"
                                              "Content-Length: 21\r\n"
                                              "\r\n"
                                              "Internal Server Error";

// 501 Not Implemented
static constexpr const char* NOT_IMPLEMENTED = "HTTP/1.1 501 Not Implemented\r\n"
                                               "Content-Type: text/plain; charset=UTF-8\r\n"
                                               "Connection: close\r\n"
                                               "Content-Length: 15\r\n"
                                               "\r\n"
                                               "Not Implemented";

// 503 Service Unavailable
static constexpr const char* SERVICE_UNAVAILABLE = "HTTP/1.1 503 Service Unavailable\r\n"
                                                   "Content-Type: text/plain; charset=UTF-8\r\n"
                                                   "Connection: close\r\n"
                                                   "Content-Length: 19\r\n"
                                                   "\r\n"
                                                   "Service Unavailable";

} // namespace HttpError
} // namespace WFX::Http

#endif // WFX_HTTP_ERROR_MESSAGES_HPP