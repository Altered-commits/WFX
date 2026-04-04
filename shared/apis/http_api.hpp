#ifndef WFX_SHARED_HTTP_API_HPP
#define WFX_SHARED_HTTP_API_HPP

#include "http/constants/http_constants.hpp"
#include "shared/http/common.hpp"
#include "shared/abis/any.hpp"
#include "shared/abis/string_view.hpp"

// Fwd declare stuff
namespace WFX::Http {

class Router;
class HttpMiddleware;
class HttpConnectionHandler;

} // namespace WFX::Http

namespace WFX::Shared {

using namespace WFX::Http; // For 'HttpMethod', ...

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
using RegisterRouteFn   = void (*)(HttpMethod method, StringView path, HttpCallbackType callback);
using RegisterRouteExFn = void (*)(HttpMethod method, StringView path, HttpMiddlewareStack mwStack, HttpCallbackType callback);
using PushRoutePrefixFn = void (*)(StringView prefix);
using PopRoutePrefixFn  = void (*)();

// Middleware
using RegisterMiddlewareFn = void (*)(StringView name, HttpMiddlewareType callback);

// Request Control
using GetMethodFn    = HttpMethod  (*)(const void* request);
using GetVersionFn   = HttpVersion (*)(const void* request);
using GetPathFn      = StringView  (*)(const void* request);
using GetBodyFn      = StringView  (*)(const void* request);
using GetHeaderFn    = bool        (*)(const void* request, StringView key, StringView* outVal);
using SetContextFn   = void        (*)(void* request, StringView key, Any value);
using GetContextFn   = bool        (*)(const void* request, StringView key, Any* outVal);
using EraseContextFn = void        (*)(void* request, StringView key);

// Response Control
using SetStatusFn = void (*)(void* response, HttpStatus status);
using SetHeaderFn = void (*)(void* response, StringView key, StringView value);
using SendTextFn  = void (*)(void* response, StringView view);
using SendFileFn  = void (*)(void* response, StringView view, bool autoHandle404);
using StreamFn    = void (*)(void* response, StreamGenerator generator, bool streamChunked);

// Endpoint API
using AllocateEndpointFn = std::uint16_t (*)(
    StringView url, std::uint32_t cLimit, std::uint32_t ifLimit, EndpointTLSConfig tlsConfig
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

    // Request Control
    GetMethodFn             GetMethod;
    GetVersionFn            GetVersion;
    GetPathFn               GetPath;
    GetBodyFn               GetBody;
    GetHeaderFn             GetHeader;
    SetContextFn            SetContext;
    GetContextFn            GetContext;
    EraseContextFn          EraseContext;

    // Response Control
    SetStatusFn             SetStatus;
    SetHeaderFn             SetHeader;
    SendTextFn              SendText;
    SendFileFn              SendFile;
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
