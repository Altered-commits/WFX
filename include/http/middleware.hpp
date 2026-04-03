#ifndef WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP
#define WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP

#include "response.hpp"
#include "helper.hpp"
#include "core/core.hpp"
#include "core/deferred_init_vector.hpp"

#define WFX_MW_CLASS(id)    WFX_CONCAT(WFXMiddleware_, id)
#define WFX_MW_INSTANCE(id) WFX_CONCAT(WFXMiddlewareInst_, id)

// Generate once
#define WFX_INTERNAL_MW_REGISTER_IMPL(name, cb, uniq)                  \
    namespace {                                                        \
        struct WFX_MW_CLASS(uniq) {                                    \
            WFX_MW_CLASS(uniq)() {                                     \
                WFX::Shared::__WFXDeferredSimple.emplace_back([] {     \
                    __WFXApi->GetHttpAPIV1()->RegisterMiddleware(      \
                        WFX::Shared::StringView::FromCString(name),    \
                        WFX::Http::MakeMiddlewareEntry(cb)             \
                    );                                                 \
                });                                                    \
            }                                                          \
        } WFX_MW_INSTANCE(uniq);                                       \
    }

#define WFX_INTERNAL_MW_REGISTER(name, cb)                             \
    WFX_INTERNAL_MW_REGISTER_IMPL(                                     \
        name, cb, __COUNTER__                                          \
    )

// vvv User friendly Macros vvv
#define WFX_MIDDLEWARE(name, cb) WFX_INTERNAL_MW_REGISTER(name, cb)

// vvv Helper Macros vvv
#define WFX_MW_LIST(...) WFX::Http::MakeMiddlewareFromFunctions(__VA_ARGS__)

#endif // WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP