#ifndef WFX_HTTP_RESPONSE_HPP
#define WFX_HTTP_RESPONSE_HPP

#include "http/constants/http_constants.hpp"
#include "http/headers/http_headers.hpp"
#include "http/common/http_route_common.hpp"

#include <nlohmann/json_fwd.hpp>

#include <variant>
#include <string>

// To keep naming consistent :)
using Json = nlohmann::json;

namespace WFX::Http {

// Forward declare connection handler here, we will include its impl in .cpp file
class HttpConnectionHandler;

using BodyType = std::variant<std::monostate, std::string_view, std::string, StreamGenerator>;

enum class OperationType : std::uint8_t {
    TEXT,
    FILE,
    STREAM
};

struct HttpResponse {
public:
    HttpResponse& Status(HttpStatus code);
    HttpResponse& Set(std::string&& key, std::string&& value);

    bool IsFileOperation()   const;
    bool IsStreamOperation() const;

    void SendText(const char* cstr);
    void SendText(std::string&& str);

    void SendJson(const Json& j);
    void SendJson(Json&& j);

    void SendFile(const char* cstr, bool autoHandle404);
    void SendFile(std::string&& path, bool autoHandle404);

    void SendTemplate(const char* cstr, bool autoHandle404);
    void SendTemplate(std::string&& path, bool autoHandle404);

    // Stream API
    void StreamFile(const char* cstr, bool autoHandle404);
    void StreamFile(std::string&& path, bool autoHandle404);
    void Stream(StreamGenerator generator, bool skipChecks = false);

private:
    void SetTextBody(std::string&& text, const char* contentType);
    void PrepareFileHeaders(std::string_view path);
    bool ValidateFileSend(std::string_view path, bool autoHandle404, const char* funcName = "SendFile()");

public:
    HttpVersion     version = HttpVersion::HTTP_1_1;
    HttpStatus      status  = HttpStatus::OK;
    ResponseHeaders headers;
    BodyType        body;

private:
    OperationType operationType_ = OperationType::TEXT;
};

} // namespace WFX::Http

#endif // WFX_HTTP_RESPONSE_HPP