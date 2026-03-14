#ifndef WFX_SHARED_HTTP_API_HPP
#define WFX_SHARED_HTTP_API_HPP

#include "http/constants/http_constants.hpp"
#include "third_party/json/json_fwd.hpp"
#include "shared/http/common.hpp"

// To be consistent with naming
using Json = nlohmann::json;

namespace WFX::Shared {

using namespace WFX::Http; // For 'HttpMethod', 'HttpResponse', 'HttpStatus'

enum class HttpAPIVersion : std::uint8_t {
    V1 = 1,
};

// Endpoint enum
enum class EndpointTLSConfig : std::uint8_t {
    AUTO,           // TLS automatically on some preconfigured ports
    FORCE_REQUIRE,  // Force TLS (Port doesn't matter)
    FORCE_INSECURE  // Explicitly allow no TLS even on secure ports
};

// Data internally used by Http API
struct HttpAPIDataV1 {
    Router*                router      = nullptr;
    HttpMiddleware*        middleware  = nullptr;
    HttpConnectionHandler* connHandler = nullptr;
    void*                  data        = nullptr;  // Any data type erased
};

// vvv All aliases for clarity vvv
// Routing
using RegisterRouteFn         = void (*)(HttpMethod method, std::string_view path, HttpCallbackType callback);
using RegisterRouteExFn       = void (*)(HttpMethod method, std::string_view path, HttpMiddlewareStack mwStack, HttpCallbackType callback);
using PushRoutePrefixFn       = void (*)(std::string_view prefix);
using PopRoutePrefixFn        = void (*)();

// Middleware
using RegisterMiddlewareFn    = void (*)(std::string_view name, HttpMiddlewareType callback);

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
using SendFileRvalueFn        = WFX::Utils::MoveOnlyFunction<void(HttpResponse*, std::string&&, bool)>;
using SendTemplateRvalueFn    = WFX::Utils::MoveOnlyFunction<void(HttpResponse*, std::string&&, Json&&)>;

// Stream API
using StreamFn = void (*)(HttpResponse* backend, StreamGenerator generator, bool streamChunked);

// Endpoint API
using AllocateEndpointFn = std::uint16_t (*)(
    std::string_view url, std::uint32_t cLimit, std::uint32_t ifLimit, EndpointTLSConfig tlsConfig
);
using WriteEndpointFn = EndpointStatus (*)(
    void* ctx, std::uint16_t endpointIndex, const std::byte* ptr, std::uint32_t size
);

// Data API
using SetGlobalPtrDataFn = void  (*)(void*);
using GetGlobalPtrDataFn = void* (*)();

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

    // SendFile overloads
    SendFileCStrFn          SendFileCStr;
    SendFileRvalueFn        SendFileMove;

    // SendTemplate overloads
    SendTemplateCStrFn      SendTemplateCStr;
    SendTemplateRvalueFn    SendTemplateMove;

    // Stream API
    StreamFn                Stream;

    // Endpoint API
    AllocateEndpointFn      AllocateEndpoint;
    WriteEndpointFn         WriteEndpoint;

    // Data API
    SetGlobalPtrDataFn      SetGlobalPtrData;
    GetGlobalPtrDataFn      GetGlobalPtrData;

    // Metadata
    HttpAPIVersion          apiVersion;
};

// vvv Getter & Initializers vvv
const HTTP_API_TABLE* GetHttpAPIV1();
void                  InitHttpAPIV1(HttpConnectionHandler*, Router*, HttpMiddleware*);

} // namespace WFX::Shared

#endif // WFX_SHARED_HTTP_API_HPP
