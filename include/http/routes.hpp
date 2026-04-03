#ifndef WFX_INC_HTTP_ROUTE_MACROS_HPP
#define WFX_INC_HTTP_ROUTE_MACROS_HPP

#include "helper.hpp"
#include "request.hpp"
#include "response.hpp"
#include "core/core.hpp"
#include "core/deferred_init_vector.hpp"

// Glue suffix to names
#define WFX_ROUTE_CLASS(prefix, id) WFX_CONCAT(WFXRoute_, WFX_CONCAT(prefix, id))
#define WFX_ROUTE_INSTANCE(id)      WFX_CONCAT(WFXRouteInst_, id)

// Generate once
#define WFX_INTERNAL_ROUTE_REGISTER_IMPL(method, path, cb, uniq)        \
    namespace {                                                         \
        struct WFX_ROUTE_CLASS(method, uniq) {                          \
            WFX_ROUTE_CLASS(method, uniq)() {                           \
                WFX::Shared::__WFXDeferredSimple.emplace_back([] {      \
                    __WFXApi->GetHttpAPIV1()->RegisterRoute(            \
                        WFX::Http::HttpMethod::method,                  \
                        WFX::Shared::StringView::FromCString(path),     \
                        WFX::Http::MakeHttpCallbackFromLambda(cb)       \
                    );                                                  \
                });                                                     \
            }                                                           \
        } WFX_ROUTE_INSTANCE(uniq);                                     \
    }

#define WFX_INTERNAL_ROUTE_REGISTER_EX_IMPL(method, path, mw, cb, uniq)       \
    namespace {                                                               \
        struct WFX_ROUTE_CLASS(method, uniq) {                                \
            WFX_ROUTE_CLASS(method, uniq)() {                                 \
                WFX::Shared::__WFXDeferredSimple.emplace_back([] {            \
                    __WFXApi->GetHttpAPIV1()->RegisterRouteEx(                \
                        WFX::Http::HttpMethod::method,                        \
                        WFX::Shared::StringView::FromCString(path),           \
                        mw,                                                   \
                        WFX::Http::MakeHttpCallbackFromLambda(cb)             \
                    );                                                        \
                });                                                           \
            }                                                                 \
        } WFX_ROUTE_INSTANCE(uniq);                                           \
    }

#define WFX_INTERNAL_ROUTE_REGISTER(method, path, cb)             \
    WFX_INTERNAL_ROUTE_REGISTER_IMPL(method, path, cb, __COUNTER__)

#define WFX_INTERNAL_ROUTE_REGISTER_EX(method, path, mw, cb)      \
    WFX_INTERNAL_ROUTE_REGISTER_EX_IMPL(method, path, mw, cb, __COUNTER__)

// vvv HTTP MACROS vvv
#define WFX_GET(path, cb)  WFX_INTERNAL_ROUTE_REGISTER(GET, path, cb)
#define WFX_POST(path, cb) WFX_INTERNAL_ROUTE_REGISTER(POST, path, cb)

#define WFX_GET_EX(path, mw, cb)  WFX_INTERNAL_ROUTE_REGISTER_EX(GET, path, mw, cb)
#define WFX_POST_EX(path, mw, cb) WFX_INTERNAL_ROUTE_REGISTER_EX(POST, path, mw, cb)

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

// vvv PATH SEGMENT HELPERS vvv
/*
 * Note: Used inside of function so typing style would be PascalCase not UPPER_SNAKE_CASE
 */
#define GetSegmentAsString(segment) std::get<std::string_view>(segment)
#define GetSegmentAsInt(segment)    std::get<std::int64_t>(segment)
#define GetSegmentAsUInt(segment)   std::get<std::uint64_t>(segment)
#define GetSegmentAsUUID(segment)   std::get<WFX::Utils::UUID>(segment)

#endif // WFX_INC_HTTP_ROUTE_MACROS_HPP