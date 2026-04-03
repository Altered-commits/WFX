#ifndef WFX_INC_HTTP_DEFERRED_INIT_VECTOR_HPP
#define WFX_INC_HTTP_DEFERRED_INIT_VECTOR_HPP

#include <functional>
#include <vector>

namespace WFX::Http {

using DeferredSimpleCallback  = void(*)();
using DeferredContextCallback = std::function<void()>;

using DeferredVectorSimple  = std::vector<DeferredSimpleCallback>;
using DeferredVectorContext = std::vector<DeferredContextCallback>;

// vvv Global Registries vvv
inline DeferredVectorSimple  __WFXDeferredSimple;
inline DeferredVectorContext __WFXDeferredContextual;

// vvv Helper Functions vvv
inline void __ExecuteAndEraseDeferred()
{
    // Run simple tasks
    for(auto func : __WFXDeferredSimple)
        func();

    __WFXDeferredSimple.clear();
    __WFXDeferredSimple.shrink_to_fit();

    // Run contextual tasks
    for(const auto& func : __WFXDeferredContextual)
        func();

    __WFXDeferredContextual.clear();
    __WFXDeferredContextual.shrink_to_fit();
}

} // namespace WFX::Http

#endif // WFX_INC_HTTP_DEFERRED_INIT_VECTOR_HPP