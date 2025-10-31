#ifndef WFX_INC_CORE_CONSTRUCTOR_MACROS_HPP
#define WFX_INC_CORE_CONSTRUCTOR_MACROS_HPP

#include "core.hpp"
#include "shared/utils/deferred_init_vector.hpp"

#define WFX_CNSTRCT_CLASS(id)    WFX_CONCAT(WFXConstructor_, id)
#define WFX_CNSTRCT_INSTANCE(id) WFX_CONCAT(WFXConstructorInst_, id)

// Generate once
#define WFX_INTERNAL_CNSTRCT_REGISTER_IMPL(callback, uniq)       \
    static struct WFX_CNSTRCT_CLASS(uniq) {                      \
        WFX_CNSTRCT_CLASS(uniq)() {                              \
            WFX::Shared::__WFXDeferredConstructors()             \
                .emplace_back([] callback);                      \
        }                                                        \
    } WFX_CNSTRCT_INSTANCE(uniq);

#define WFX_INTERNAL_CNSTRCT_REGISTER(callback)                  \
    WFX_INTERNAL_CNSTRCT_REGISTER_IMPL(callback, __COUNTER__)

// User-friendly macro
#define WFX_CONSTRUCTOR(cb) WFX_INTERNAL_CNSTRCT_REGISTER(cb)

#endif // WFX_INC_CORE_CONSTRUCTOR_MACROS_HPP