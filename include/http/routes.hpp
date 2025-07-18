#ifndef WFX_HTTP_ROUTE_MACROS_HPP
#define WFX_HTTP_ROUTE_MACROS_HPP

#include "router.hpp"
#include "http/request/http_request.hpp"
#include "http/response/http_response.hpp"
#include "shared/apis/master_api.hpp"
#include "shared/utils/export_macro.hpp"
#include "shared/utils/deferred_init_vector.hpp"

// Declare the injected API table
extern const WFX::Shared::MASTER_API_TABLE* __wfx_api;

// Pre-fetch the HttpAPI (safe since this is only compiled in user DLL)
inline const WFX::Shared::HTTP_API_TABLE* __wfx_http_api = 
    (__wfx_api && __wfx_api->GetHttpAPIV1()) ? __wfx_api->GetHttpAPIV1() : nullptr;

// For users 'ease of use'
using Request  = WFX::Http::HttpRequest;
using Response = WFX::Http::HttpResponse;

// vvv MACROS HELPERS vvv
#define WFX_ROUTE_UNIQUE_NAME(prefix) WFXRoute_ ## prefix ## __ ## __COUNTER__

#define WFX_INTERNAL_ROUTE_REGISTER(method, path, callback)              \
    struct WFX_ROUTE_UNIQUE_NAME(method) {                               \
        WFX_ROUTE_UNIQUE_NAME(method)() {                                \
            WFX::Shared::__wfx_deferred_routes().emplace_back([] {       \
                if(__wfx_http_api)                                       \
                    __wfx_http_api->RegisterRoute(                       \
                        WFX::Http::HttpMethod::method, path, callback    \
                    );                                                   \
            });                                                          \
        }                                                                \
    };                                                                   \
    WFX_EXPORT WFX_ROUTE_UNIQUE_NAME(method) WFX_ROUTE_UNIQUE_NAME(Inst);

// vvv MACROS FOR USER vvv
#define WFX_GET(path, cb)  WFX_INTERNAL_ROUTE_REGISTER(GET, path, cb)
#define WFX_POST(path, cb) WFX_INTERNAL_ROUTE_REGISTER(POST, path, cb)

#endif // WFX_HTTP_ROUTE_MACROS_HPP