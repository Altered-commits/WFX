#ifndef WFX_HTTP_CONSTANTS_HPP
#define WFX_HTTP_CONSTANTS_HPP

#include <string_view>

namespace WFX::Http {

// We will keep it quite simple, only GET and POST support for now
enum class HttpMethod {
    GET,
    POST,
    UNKNOWN
};

// HTTP Status Codes (common subset)
enum class HttpStatus {
    // 1xx Informational
    CONTINUE                    = 100,
    SWITCHING_PROTOCOLS         = 101,
    PROCESSING                  = 102,
    EARLY_HINTS                 = 103,

    // 2xx Success
    OK                          = 200,
    CREATED                     = 201,
    ACCEPTED                    = 202,
    NON_AUTHORITATIVE_INFO      = 203,
    NO_CONTENT                  = 204,
    RESET_CONTENT               = 205,
    PARTIAL_CONTENT             = 206,
    MULTI_STATUS                = 207,
    ALREADY_REPORTED            = 208,
    IM_USED                     = 226,

    // 3xx Redirection
    MULTIPLE_CHOICES            = 300,
    MOVED_PERMANENTLY           = 301,
    FOUND                       = 302,
    SEE_OTHER                   = 303,
    NOT_MODIFIED                = 304,
    USE_PROXY                  = 305,
    TEMPORARY_REDIRECT          = 307,
    PERMANENT_REDIRECT          = 308,

    // 4xx Client Error
    BAD_REQUEST                 = 400,
    UNAUTHORIZED                = 401,
    PAYMENT_REQUIRED            = 402,
    FORBIDDEN                   = 403,
    NOT_FOUND                   = 404,
    METHOD_NOT_ALLOWED          = 405,
    NOT_ACCEPTABLE              = 406,
    PROXY_AUTHENTICATION_REQUIRED = 407,
    REQUEST_TIMEOUT             = 408,
    CONFLICT                   = 409,
    GONE                        = 410,
    LENGTH_REQUIRED             = 411,
    PRECONDITION_FAILED         = 412,
    PAYLOAD_TOO_LARGE           = 413,
    URI_TOO_LONG                = 414,
    UNSUPPORTED_MEDIA_TYPE      = 415,
    RANGE_NOT_SATISFIABLE       = 416,
    EXPECTATION_FAILED          = 417,
    IM_A_TEAPOT                 = 418,
    MISDIRECTED_REQUEST         = 421,
    UNPROCESSABLE_ENTITY        = 422,
    LOCKED                      = 423,
    FAILED_DEPENDENCY           = 424,
    TOO_EARLY                   = 425,
    UPGRADE_REQUIRED            = 426,
    PRECONDITION_REQUIRED       = 428,
    TOO_MANY_REQUESTS           = 429,
    REQUEST_HEADER_FIELDS_TOO_LARGE = 431,
    UNAVAILABLE_FOR_LEGAL_REASONS  = 451,

    // 5xx Server Error
    INTERNAL_SERVER_ERROR       = 500,
    NOT_IMPLEMENTED             = 501,
    BAD_GATEWAY                 = 502,
    SERVICE_UNAVAILABLE         = 503,
    GATEWAY_TIMEOUT             = 504,
    HTTP_VERSION_NOT_SUPPORTED  = 505,
    VARIANT_ALSO_NEGOTIATES     = 506,
    INSUFFICIENT_STORAGE        = 507,
    LOOP_DETECTED               = 508,
    NOT_EXTENDED                = 510,
    NETWORK_AUTHENTICATION_REQUIRED = 511
};

enum class HttpVersion {
    HTTP_1_0,
    HTTP_1_1,
    HTTP_2_0,
    UNKNOWN
};

// Helper functions
// ENUM -> STRING
static inline const char* HttpStatusToReason(HttpStatus code)
{
    switch(code) {
        // 1xx Informational
        case HttpStatus::CONTINUE:                         return "Continue";
        case HttpStatus::SWITCHING_PROTOCOLS:              return "Switching Protocols";
        case HttpStatus::PROCESSING:                       return "Processing";
        case HttpStatus::EARLY_HINTS:                      return "Early Hints";

        // 2xx Success
        case HttpStatus::OK:                               return "OK";
        case HttpStatus::CREATED:                          return "Created";
        case HttpStatus::ACCEPTED:                         return "Accepted";
        case HttpStatus::NON_AUTHORITATIVE_INFO:           return "Non-Authoritative Information";
        case HttpStatus::NO_CONTENT:                       return "No Content";
        case HttpStatus::RESET_CONTENT:                    return "Reset Content";
        case HttpStatus::PARTIAL_CONTENT:                  return "Partial Content";
        case HttpStatus::MULTI_STATUS:                     return "Multi-Status";
        case HttpStatus::ALREADY_REPORTED:                 return "Already Reported";
        case HttpStatus::IM_USED:                          return "IM Used";

        // 3xx Redirection
        case HttpStatus::MULTIPLE_CHOICES:                 return "Multiple Choices";
        case HttpStatus::MOVED_PERMANENTLY:                return "Moved Permanently";
        case HttpStatus::FOUND:                            return "Found";
        case HttpStatus::SEE_OTHER:                        return "See Other";
        case HttpStatus::NOT_MODIFIED:                     return "Not Modified";
        case HttpStatus::USE_PROXY:                        return "Use Proxy";
        case HttpStatus::TEMPORARY_REDIRECT:               return "Temporary Redirect";
        case HttpStatus::PERMANENT_REDIRECT:               return "Permanent Redirect";

        // 4xx Client Errors
        case HttpStatus::BAD_REQUEST:                      return "Bad Request";
        case HttpStatus::UNAUTHORIZED:                     return "Unauthorized";
        case HttpStatus::PAYMENT_REQUIRED:                 return "Payment Required";
        case HttpStatus::FORBIDDEN:                        return "Forbidden";
        case HttpStatus::NOT_FOUND:                        return "Not Found";
        case HttpStatus::METHOD_NOT_ALLOWED:               return "Method Not Allowed";
        case HttpStatus::NOT_ACCEPTABLE:                   return "Not Acceptable";
        case HttpStatus::PROXY_AUTHENTICATION_REQUIRED:    return "Proxy Authentication Required";
        case HttpStatus::REQUEST_TIMEOUT:                  return "Request Timeout";
        case HttpStatus::CONFLICT:                         return "Conflict";
        case HttpStatus::GONE:                             return "Gone";
        case HttpStatus::LENGTH_REQUIRED:                  return "Length Required";
        case HttpStatus::PRECONDITION_FAILED:              return "Precondition Failed";
        case HttpStatus::PAYLOAD_TOO_LARGE:                return "Payload Too Large";
        case HttpStatus::URI_TOO_LONG:                     return "URI Too Long";
        case HttpStatus::UNSUPPORTED_MEDIA_TYPE:           return "Unsupported Media Type";
        case HttpStatus::RANGE_NOT_SATISFIABLE:            return "Range Not Satisfiable";
        case HttpStatus::EXPECTATION_FAILED:               return "Expectation Failed";
        case HttpStatus::IM_A_TEAPOT:                      return "I'm a teapot";
        case HttpStatus::MISDIRECTED_REQUEST:              return "Misdirected Request";
        case HttpStatus::UNPROCESSABLE_ENTITY:             return "Unprocessable Entity";
        case HttpStatus::LOCKED:                           return "Locked";
        case HttpStatus::FAILED_DEPENDENCY:                return "Failed Dependency";
        case HttpStatus::TOO_EARLY:                        return "Too Early";
        case HttpStatus::UPGRADE_REQUIRED:                 return "Upgrade Required";
        case HttpStatus::PRECONDITION_REQUIRED:            return "Precondition Required";
        case HttpStatus::TOO_MANY_REQUESTS:                return "Too Many Requests";
        case HttpStatus::REQUEST_HEADER_FIELDS_TOO_LARGE:  return "Request Header Fields Too Large";
        case HttpStatus::UNAVAILABLE_FOR_LEGAL_REASONS:    return "Unavailable For Legal Reasons";

        // 5xx Server Errors
        case HttpStatus::INTERNAL_SERVER_ERROR:            return "Internal Server Error";
        case HttpStatus::NOT_IMPLEMENTED:                  return "Not Implemented";
        case HttpStatus::BAD_GATEWAY:                      return "Bad Gateway";
        case HttpStatus::SERVICE_UNAVAILABLE:              return "Service Unavailable";
        case HttpStatus::GATEWAY_TIMEOUT:                  return "Gateway Timeout";
        case HttpStatus::HTTP_VERSION_NOT_SUPPORTED:       return "HTTP Version Not Supported";
        case HttpStatus::VARIANT_ALSO_NEGOTIATES:          return "Variant Also Negotiates";
        case HttpStatus::INSUFFICIENT_STORAGE:             return "Insufficient Storage";
        case HttpStatus::LOOP_DETECTED:                    return "Loop Detected";
        case HttpStatus::NOT_EXTENDED:                     return "Not Extended";
        case HttpStatus::NETWORK_AUTHENTICATION_REQUIRED:  return "Network Authentication Required";

        default: return "Unknown";
    }
}

// STRING -> ENUM
static inline HttpMethod HttpMethodToEnum(std::string_view method)
{
    if(method == "GET")  return HttpMethod::GET;
    if(method == "POST") return HttpMethod::POST;
    
    return HttpMethod::UNKNOWN;
}

static inline HttpVersion HttpVersionToEnum(std::string_view version)
{
    if(version == "HTTP/1.1") return HttpVersion::HTTP_1_1;
    if(version == "HTTP/1.0") return HttpVersion::HTTP_1_0;
    if(version == "HTTP/2.0") return HttpVersion::HTTP_2_0;
    
    return HttpVersion::UNKNOWN;
}

} // namespace WFX::Http

#endif // WFX_HTTP_CONSTANTS_HPP