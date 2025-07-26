#ifndef WFX_INC_HTTP_ROUTE_MACROS_HPP
#define WFX_INC_HTTP_ROUTE_MACROS_HPP

#include "aliases.hpp"
#include "response.hpp"

#include "shared/apis/master_api.hpp"
#include "shared/utils/export_macro.hpp"
#include "shared/utils/deferred_init_vector.hpp"

// Declare the injected API table
extern const WFX::Shared::MASTER_API_TABLE* __wfx_api;

// vvv MACROS HELPERS vvv
#define WFX_CONCAT_INNER(a, b) a##b
#define WFX_CONCAT(a, b) WFX_CONCAT_INNER(a, b)

// Glue suffix to names
#define WFX_ROUTE_CLASS(prefix, id) WFX_CONCAT(WFXRoute_, WFX_CONCAT(prefix, id))
#define WFX_ROUTE_INSTANCE(id)      WFX_CONCAT(WFXRouteInst_, id)

// Generate once
#define WFX_INTERNAL_ROUTE_REGISTER_IMPL(method, path, callback, uniq)  \
    struct WFX_ROUTE_CLASS(method, uniq) {                              \
        WFX_ROUTE_CLASS(method, uniq)() {                               \
            WFX::Shared::__WFXDeferredRoutes().emplace_back([] {      \
                if(__wfx_api->GetHttpAPIV1())                           \
                    __wfx_api->GetHttpAPIV1()->RegisterRoute(           \
                        WFX::Http::HttpMethod::method, path, callback   \
                    );                                                  \
            });                                                         \
        }                                                               \
    } WFX_ROUTE_INSTANCE(uniq);

#define WFX_INTERNAL_ROUTE_REGISTER(method, path, callback)             \
    WFX_INTERNAL_ROUTE_REGISTER_IMPL(method, path, callback, __COUNTER__)

// vvv HTTP MACROS vvv
#define WFX_GET(path, cb)  WFX_INTERNAL_ROUTE_REGISTER(GET, path, cb)
#define WFX_POST(path, cb) WFX_INTERNAL_ROUTE_REGISTER(POST, path, cb)

// vvv ROUTE GROUPING vvv
#define WFX_GROUP_START_IMPL(path, id)                                \
    static struct WFX_CONCAT(WFXGroupStart_, id) {                    \
        WFX_CONCAT(WFXGroupStart_, id)() {                            \
            WFX::Shared::__WFXDeferredRoutes().emplace_back([] {    \
                if(__wfx_api->GetHttpAPIV1())                         \
                    __wfx_api->GetHttpAPIV1()->PushRoutePrefix(path); \
            });                                                       \
        }                                                             \
    } WFX_CONCAT(WFXGroupStartInst_, id);

#define WFX_GROUP_END_IMPL(id)                                        \
    static struct WFX_CONCAT(WFXGroupEnd_, id) {                      \
        WFX_CONCAT(WFXGroupEnd_, id)() {                              \
            WFX::Shared::__WFXDeferredRoutes().emplace_back([] {    \
                if(__wfx_api->GetHttpAPIV1())                         \
                    __wfx_api->GetHttpAPIV1()->PopRoutePrefix();      \
            });                                                       \
        }                                                             \
    } WFX_CONCAT(WFXGroupEndInst_, id);

#define WFX_GROUP_START(path) WFX_GROUP_START_IMPL(path, __COUNTER__)
#define WFX_GROUP_END()       WFX_GROUP_END_IMPL(__COUNTER__)

#endif // WFX_INC_HTTP_ROUTE_MACROS_HPP