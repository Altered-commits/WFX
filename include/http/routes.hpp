#ifndef WFX_INC_HTTP_ROUTE_MACROS_HPP
#define WFX_INC_HTTP_ROUTE_MACROS_HPP

#include "helper.hpp"
#include "response.hpp"
#include "request.hpp"
#include "core/core.hpp"
#include "core/deferred_init_vector.hpp"

// Glue suffix to names
#define WFX_ROUTE_CLASS(prefix, id) WFX_CONCAT(WFXRoute_, WFX_CONCAT(prefix, id))
#define WFX_ROUTE_INSTANCE(id)      WFX_CONCAT(WFXRouteInst_, id)

#define WFX_INTERNAL_ROUTE_REGISTER_IMPL(method, path, callback, uniq)        \
    namespace {                                                               \
        struct WFX_ROUTE_CLASS(method, uniq) {                                \
            WFX_ROUTE_CLASS(method, uniq)() {                                 \
                WFX::Shared::__WFXDeferred.emplace_back([] {                  \
                    __WFXApi->GetHttpAPIV1()->RegisterRoute(                  \
                        WFX::Http::HttpMethod::method,                        \
                        WFX::Shared::StringView::FromCString(path),           \
                        WFX::Http::MakeRouteCallback(callback)                \
                    );                                                        \
                });                                                           \
            }                                                                 \
        } WFX_ROUTE_INSTANCE(uniq);                                           \
    }

#define WFX_INTERNAL_ROUTE_REGISTER_EX_IMPL(method, path, mw, callback, uniq) \
    namespace {                                                               \
        struct WFX_ROUTE_CLASS(method, uniq) {                                \
            WFX_ROUTE_CLASS(method, uniq)() {                                 \
                auto mwArr = mw;                                              \
                WFX::Shared::__WFXDeferred.emplace_back(                      \
                    [mwArr]() mutable {                                       \
                        __WFXApi->GetHttpAPIV1()->RegisterRouteEx(            \
                            WFX::Http::HttpMethod::method,                    \
                            WFX::Shared::StringView::FromCString(path),       \
                            mwArr.data(), mwArr.count(),                      \
                            WFX::Http::MakeRouteCallback(callback)            \
                        );                                                    \
                    }                                                         \
                );                                                            \
            }                                                                 \
        } WFX_ROUTE_INSTANCE(uniq);                                           \
    }

#define WFX_INTERNAL_ROUTE_REGISTER(method, path, callback)             \
    WFX_INTERNAL_ROUTE_REGISTER_IMPL(method, path, callback, __COUNTER__)

#define WFX_INTERNAL_ROUTE_REGISTER_EX(method, path, mw, callback)      \
    WFX_INTERNAL_ROUTE_REGISTER_EX_IMPL(method, path, mw, callback, __COUNTER__)

// vvv HTTP MACROS vvv
// Simple routes
#define WFX_GET(path, cb)     WFX_INTERNAL_ROUTE_REGISTER(GET, path, cb)
#define WFX_POST(path, cb)    WFX_INTERNAL_ROUTE_REGISTER(POST, path, cb)

// Routes with per-route middleware
// Usage: WFX_GET_EX("/path", WFX::Http::MakeMiddleware(mw1, mw2), handler)
#define WFX_GET_EX(path, mw, cb)     WFX_INTERNAL_ROUTE_REGISTER_EX(GET, path, mw, cb)
#define WFX_POST_EX(path, mw, cb)    WFX_INTERNAL_ROUTE_REGISTER_EX(POST, path, mw, cb)

// vvv ROUTE GROUPING vvv
#define WFX_GROUP_START_IMPL(path, id)                                \
    namespace {                                                       \
        struct WFX_CONCAT(WFXGroupStart_, id) {                       \
            WFX_CONCAT(WFXGroupStart_, id)() {                        \
                WFX::Shared::__WFXDeferredSimple.emplace_back([] {    \
                    __WFXApi->GetHttpAPIV1()->PushRoutePrefix(path);  \
                });                                                   \
            }                                                         \
        } WFX_CONCAT(WFXGroupStartInst_, id);                         \
    }

#define WFX_GROUP_END_IMPL(id)                                        \
    namespace {                                                       \
        struct WFX_CONCAT(WFXGroupEnd_, id) {                         \
            WFX_CONCAT(WFXGroupEnd_, id)() {                          \
                WFX::Shared::__WFXDeferredSimple.emplace_back([] {    \
                    __WFXApi->GetHttpAPIV1()->PopRoutePrefix();       \
                });                                                   \
            }                                                         \
        } WFX_CONCAT(WFXGroupEndInst_, id);                           \
    }

#define WFX_GROUP_START(path) WFX_GROUP_START_IMPL(path, __COUNTER__)
#define WFX_GROUP_END()       WFX_GROUP_END_IMPL(__COUNTER__)

#endif // WFX_INC_HTTP_ROUTE_MACROS_HPP