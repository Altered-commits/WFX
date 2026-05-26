#ifndef WFX_INC_CORE_DEFERRED_INIT_VECTOR_HPP
#define WFX_INC_CORE_DEFERRED_INIT_VECTOR_HPP

#include <functional>
#include <vector>

namespace WFX::Core {

using DeferredCallback = std::function<void()>;
using DeferredVector = std::vector<DeferredCallback>;

// vvv Global Registries vvv
inline DeferredVector __WFXDeferred;

// vvv Helper Functions vvv
inline void __ExecuteAndEraseDeferred()
{
    // Run tasks
    for(const auto& func : __WFXDeferred)
        func();

    __WFXDeferred.clear();
    __WFXDeferred.shrink_to_fit();
}

} // namespace WFX::Core

#endif // WFX_INC_CORE_DEFERRED_INIT_VECTOR_HPP