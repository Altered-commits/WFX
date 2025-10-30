#ifndef WFX_SHARED_HTTP_API_HPP
#define WFX_SHARED_HTTP_API_HPP

#include "http/constants/http_constants.hpp"
#include "http/common/http_route_common.hpp"
#include "third_party/json/json_fwd.hpp"

// To be consistent with naming
using Json = nlohmann::json;

namespace WFX::Shared {

using namespace WFX::Http; // For 'HttpMethod', 'HttpResponse', 'HttpStatus'

enum class HttpAPIVersion : std::uint8_t {
    V1 = 1,
};

// vvv All aliases for clarity vvv
// Routing
using RegisterRouteFn         = void (*)(HttpMethod method, std::string_view path, HttpCallbackType callback);
using RegisterRouteExFn       = void (*)(HttpMethod method, std::string_view path, MiddlewareStack mwStack, HttpCallbackType callback);
using PushRoutePrefixFn       = void (*)(std::string_view prefix);
using PopRoutePrefixFn        = void (*)();

// Middleware
using RegisterMiddlewareFn    = void (*)(std::string_view name, MiddlewareCallbackType callback);

// Response control
using SetStatusFn             = void (*)(HttpResponse* backend, HttpStatus status);
using SetHeaderFn             = void (*)(HttpResponse* backend, std::string key, std::string value);

// SendText
using SendTextCStrFn          = void (*)(HttpResponse* backend, const char* cstr);

// SendJson
using SendJsonConstRefFn      = void (*)(HttpResponse* backend, const Json* json);

// SendFile
using SendFileCStrFn          = void (*)(HttpResponse* backend, const char* cstr, bool autoHandle404);

// SendTemplate
using SendTemplateCStrFn      = void (*)(HttpResponse* backend, const char* cstr, Json&& ctx);

// Special rvalue overload
using SendTextRvalueFn        = WFX::Utils::MoveOnlyFunction<void(HttpResponse*, std::string&&)>;
using SendJsonRvalueFn        = WFX::Utils::MoveOnlyFunction<void(HttpResponse*, Json&&)>;
using SendFileRvalueFn        = WFX::Utils::MoveOnlyFunction<void(HttpResponse*, std::string&&, bool)>;
using SendTemplateRvalueFn    = WFX::Utils::MoveOnlyFunction<void(HttpResponse*, std::string&&, Json&&)>;

// Stream API
using StreamFn = void (*)(HttpResponse* backend, StreamGenerator generator, bool streamChunked);

// vvv API declarations vvv
struct HTTP_API_TABLE {
    // Routing
    RegisterRouteFn         RegisterRoute;
    RegisterRouteExFn       RegisterRouteEx;
    PushRoutePrefixFn       PushRoutePrefix;
    PopRoutePrefixFn        PopRoutePrefix;

    // Middleware
    RegisterMiddlewareFn    RegisterMiddleware;

    // Response manipulation
    SetStatusFn             SetStatus;
    SetHeaderFn             SetHeader;

    // SendText overloads
    SendTextCStrFn          SendTextCStr;
    SendTextRvalueFn        SendTextMove;

    // SendJson overloads
    SendJsonConstRefFn      SendJsonConstRef;
    SendJsonRvalueFn        SendJsonMove;

    // SendFile overloads
    SendFileCStrFn          SendFileCStr;
    SendFileRvalueFn        SendFileMove;

    // SendTemplate overloads
    SendTemplateCStrFn      SendTemplateCStr;
    SendTemplateRvalueFn    SendTemplateMove;

    // Stream API
    StreamFn                Stream;

    // Metadata
    HttpAPIVersion          apiVersion;
};

// vvv Getter vvv
const HTTP_API_TABLE* GetHttpAPIV1();

} // namespace WFX::Shared

#endif // WFX_SHARED_HTTP_API_HPP
