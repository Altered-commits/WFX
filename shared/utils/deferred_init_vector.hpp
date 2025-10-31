#ifndef WFX_SHARED_DEFERRED_INIT_VECTOR_HPP
#define WFX_SHARED_DEFERRED_INIT_VECTOR_HPP

#include <vector>
#include <functional>

namespace WFX::Shared {

using FunctionVector = std::vector<std::function<void()>>;

inline FunctionVector& __WFXDeferredConstructors()
{
    static FunctionVector constructorsReg;
    return constructorsReg;
}

inline FunctionVector& __WFXDeferredRoutes()
{
    static FunctionVector routesReg;
    return routesReg;
}

inline FunctionVector& __WFXDeferredMiddleware()
{
    static FunctionVector middlewareReg;
    return middlewareReg;
}

inline void __EraseDeferredVector(FunctionVector& deferredVector)
{
    deferredVector.clear();
    deferredVector.shrink_to_fit();
}

} // namespace WFX::Shared

#endif // WFX_SHARED_DEFERRED_INIT_VECTOR_HPP