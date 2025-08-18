#ifndef WFX_SHARED_HTTP_API_HPP
#define WFX_SHARED_HTTP_API_HPP

#include "http/constants/http_constants.hpp"
#include "http/common/route_common.hpp"
#include <nlohmann/json_fwd.hpp>

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
using PushRoutePrefixFn       = void (*)(std::string_view prefix);
using PopRoutePrefixFn        = void (*)();

// Response control
using SetStatusFn             = void (*)(HttpResponse* backend, HttpStatus status);
using SetHeaderFn             = void (*)(HttpResponse* backend, std::string key, std::string value);

// SendText
using SendTextCStrFn          = void (*)(HttpResponse* backend, const char* cstr);

// SendJson
using SendJsonConstRefFn      = void (*)(HttpResponse* backend, const Json* json);

// SendFile
using SendFileCStrFn          = void (*)(HttpResponse* backend, const char* cstr, bool);

// Special rvalue overload
using SendTextRvalueFn        = WFX::Utils::MoveOnlyFunction<void(HttpResponse*, std::string&&)>;
using SendJsonRvalueFn        = WFX::Utils::MoveOnlyFunction<void(HttpResponse*, Json&&)>;
using SendFileRvalueFn        = WFX::Utils::MoveOnlyFunction<void(HttpResponse*, std::string&&, bool)>;

// Query
using IsFileOperationFn       = bool (*)(const HttpResponse* backend);

// vvv API declarations vvv
struct HTTP_API_TABLE {
    // Routing
    RegisterRouteFn         RegisterRoute;
    PushRoutePrefixFn       PushRoutePrefix;
    PopRoutePrefixFn        PopRoutePrefix;

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

    // Metadata
    HttpAPIVersion          apiVersion;
};

// vvv Getter vvv
const HTTP_API_TABLE* GetHttpAPIV1();

} // namespace WFX::Shared

#endif // WFX_SHARED_HTTP_API_HPP
