#ifndef WFX_HTTP_RESPONSE_HPP
#define WFX_HTTP_RESPONSE_HPP

#include "http/constants/http_constants.hpp"
#include "http/headers/http_headers.hpp"
#include "http/common/http_detector.hpp"

#include "utils/filesystem/filesystem.hpp"
#include "utils/backport/string.hpp"
#include "utils/logger/logger.hpp"

#include "third_party/nlohmann/json.hpp"

#include <type_traits>
#include <charconv>
#include <string>

// To keep naming consistent :)
using Json = nlohmann::json;

namespace WFX::Http {

/*
 * NOTE: ConstHeaderValRef is defined in http_headers.hpp as const std::string&
 *       HeaderValType is defined in http_headers.hpp as std::string
 */

struct HttpResponse {
    HttpVersion     version = HttpVersion::HTTP_1_1;
    HttpStatus      status  = HttpStatus::OK;
    ResponseHeaders headers;
    std::string     body;

    // Setters
    HttpResponse& Status(HttpStatus code)
    {
        status = code; return *this;
    }

    HttpResponse& Set(const std::string& key, const std::string& value)
    {
        headers.SetHeader(key, value); return *this;
    }

    // Getters
    bool IsFileOperation() { return isFileOperation; }

    // Senders (Templated for ease of copying and moving semantics)
    template<typename T> void SendText(T&& text);
    template<typename T> void SendJson(T&& json);
    template<typename T> void SendFile(T&& path);

private: // So users can't 'accidentally' access this and set this
    bool isFileOperation = false;
};

} // namespace WFX::Http

// For template definitions
#include "http_response.ipp"

#endif // WFX_HTTP_RESPONSE_HPP