#ifndef WFX_HTTP_RESPONSE_HPP
#define WFX_HTTP_RESPONSE_HPP

#include "http/constants/http_constants.hpp"
#include "http/headers/http_headers.hpp"
#include "shared/http/common.hpp"

#include "include/third_party/json/json_fwd.hpp"

#include <variant>
#include <string>

// To keep naming consistent :)
using Json = nlohmann::json;

namespace WFX::Http {

using namespace WFX::Shared;

using BodyType = std::variant<std::monostate, std::string_view, std::string, StreamGenerator>;

enum class OperationType : std::uint8_t {
    TEXT,
    FILE,
    STREAM_CHUNKED,
    STREAM_FIXED
};

struct HttpResponse {
public:
    HttpResponse& Status(HttpStatus code);
    HttpResponse& Set(std::string&& key, std::string&& value);

    bool          IsFileOperation()   const;
    bool          IsStreamOperation() const;
    OperationType GetOperation()      const;

    void SendText(std::string_view cstr);
    void SendText(std::string&& str);

    void SendJson(const Json& j);

    void SendFile(std::string_view cstr, bool autoHandle404);
    void SendFile(std::string&& path, bool autoHandle404);

    void SendTemplate(const char* cstr, Json&& ctx);
    void SendTemplate(std::string&& path, Json&& ctx);

    // Stream API
    void Stream(StreamGenerator generator, bool streamChunked = true, bool skipChecks = false);

private:
    void SetTextBody(std::string&& text, const char* contentType);
    void PrepareFileHeaders(std::string_view path);
    bool ValidateFileSend(std::string_view path, bool autoHandle404, const char* funcName = "SendFile()");

public: // Internal use
    void ClearInfo();
    void DestroyStream(StreamGenerator& gen);

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