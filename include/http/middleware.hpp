#ifndef WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP
#define WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP

#include "helper.hpp"
#include "core/core.hpp"
#include "core/deferred_init_vector.hpp"

#define WFX_MW_CLASS(id)    WFX_CONCAT(WFXMiddleware_, id)
#define WFX_MW_INSTANCE(id) WFX_CONCAT(WFXMiddlewareInst_, id)

#define WFX_INTERNAL_MW_REGISTER_IMPL(name, callback, uniq)            \
    namespace {                                                        \
        struct WFX_MW_CLASS(uniq) {                                    \
            WFX_MW_CLASS(uniq)() {                                     \
                WFX::Shared::__WFXDeferred.emplace_back([] {           \
                    __WFXApi->GetHttpAPIV1()->RegisterMiddleware(      \
                        WFX::Shared::StringView::FromCString(name),    \
                        WFX::Http::MakeMwCallback(callback)            \
                    );                                                 \
                });                                                    \
            }                                                          \
        } WFX_MW_INSTANCE(uniq);                                       \
    }

#define WFX_INTERNAL_MW_REGISTER(name, callback)                       \
    WFX_INTERNAL_MW_REGISTER_IMPL(name, callback, __COUNTER__)

// vvv User friendly Macros vvv
#define WFX_MIDDLEWARE(name, cb) WFX_INTERNAL_MW_REGISTER(name, cb)

// vvv Per-route middleware list vvv
// Usage: WFX_GET_EX("/path", WFX_MW_LIST(mw1, mw2), handler)
#define WFX_MW_LIST(...) WFX::Http::MakeMiddleware(__VA_ARGS__)

#endif // WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP