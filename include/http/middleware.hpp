#ifndef WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP
#define WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP

#include "helper.hpp"
#include "core/core.hpp"
#include "core/deferred_init_vector.hpp"

#define WFX_MW_CLASS(id)    WFX_CONCAT(WFXMiddleware_, id)
#define WFX_MW_INSTANCE(id) WFX_CONCAT(WFXMiddlewareInst_, id)

#define WFX_INTERNAL_MW_REGISTER_IMPL(name, uniq, ...)                 \
    namespace {                                                        \
        struct WFX_MW_CLASS(uniq) {                                    \
            WFX_MW_CLASS(uniq)() {                                     \
                WFX::Core::__WFXDeferred.emplace_back([] {             \
                    WFX::Core::HttpApiExt1()->RegisterMiddleware(          \
                        WFX::Shared::StringView::FromCString(name),    \
                        WFX::Http::MakeMwCallback(__VA_ARGS__)         \
                    );                                                 \
                });                                                    \
            }                                                          \
        } WFX_MW_INSTANCE(uniq);                                       \
    }

#define WFX_INTERNAL_MW_REGISTER(name, ...)                            \
    WFX_INTERNAL_MW_REGISTER_IMPL(name, __COUNTER__, __VA_ARGS__)

// vvv User friendly macro vvv
#define WFX_MIDDLEWARE(name, ...) WFX_INTERNAL_MW_REGISTER(name, __VA_ARGS__)

// vvv Per-route middleware list vvv
// Usage: WFX_GET_EX("/path", WFX_MW_LIST(mw1, mw2), handler)
#define WFX_MW_LIST(...) WFX::Http::MakeMiddleware(__VA_ARGS__)

#endif // WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP