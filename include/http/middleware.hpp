#ifndef WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP
#define WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP

#include "aliases.hpp"
#include "response.hpp"
#include "common/core.hpp"
#include "shared/utils/deferred_init_vector.hpp"

#define WFX_MW_CLASS(id)    WFX_CONCAT(WFXMiddleware_, id)
#define WFX_MW_INSTANCE(id) WFX_CONCAT(WFXMiddlewareInst_, id)

// Generate once
#define WFX_INTERNAL_MW_REGISTER_IMPL(name, callback, uniq)            \
    static struct WFX_MW_CLASS(uniq) {                                 \
        WFX_MW_CLASS(uniq)() {                                         \
            WFX::Shared::__WFXDeferredMiddleware().emplace_back([] {   \
                __wfx_api->GetHttpAPIV1()->RegisterMiddleware(         \
                    name, callback                                     \
                );                                                     \
            });                                                        \
        }                                                              \
    } WFX_MW_INSTANCE(uniq);

#define WFX_INTERNAL_MW_REGISTER(name, callback)                       \
    WFX_INTERNAL_MW_REGISTER_IMPL(name, callback, __COUNTER__)

// User-friendly macro
#define WFX_MIDDLEWARE(name, cb) WFX_INTERNAL_MW_REGISTER(name, cb)

#endif // WFX_INC_HTTP_MIDDLEWARE_MACROS_HPP