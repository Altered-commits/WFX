#ifndef WFX_INC_CORE_CONSTRUCTOR_MACROS_HPP
#define WFX_INC_CORE_CONSTRUCTOR_MACROS_HPP

#include "core.hpp"
#include "core/deferred_init_vector.hpp"

#define WFX_CONSTRUCTOR_CLASS(id)    WFX_CONCAT(WFXConstructor_, id)
#define WFX_CONSTRUCTOR_INSTANCE(id) WFX_CONCAT(WFXConstructorInst_, id)

#define WFX_INTERNAL_CONSTRUCTOR_REGISTER_IMPL(uniq, ...)               \
    namespace {                                                         \
        struct WFX_CONSTRUCTOR_CLASS(uniq) {                            \
            WFX_CONSTRUCTOR_CLASS(uniq)() {                             \
                WFX::Core::__WFXDeferred.emplace_back(__VA_ARGS__);     \
            }                                                           \
        } WFX_CONSTRUCTOR_INSTANCE(uniq);                               \
    }

#define WFX_INTERNAL_CONSTRUCTOR_REGISTER(...)                       \
    WFX_INTERNAL_CONSTRUCTOR_REGISTER_IMPL(__COUNTER__, __VA_ARGS__)

// vvv User friendly macro vvv
#define WFX_CONSTRUCTOR(...) WFX_INTERNAL_CONSTRUCTOR_REGISTER(__VA_ARGS__)

#endif // WFX_INC_CORE_CONSTRUCTOR_MACROS_HPP