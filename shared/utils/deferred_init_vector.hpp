#ifndef WFX_SHARED_DEFERRED_INIT_VECTOR_HPP
#define WFX_SHARED_DEFERRED_INIT_VECTOR_HPP

#include <vector>
#include <functional>

namespace WFX::Shared {

inline std::vector<std::function<void()>>& __WFXDeferredRoutes()
{
    static std::vector<std::function<void()>> routesReg;
    return routesReg;
}

} // namespace WFX::Shared

#endif // WFX_SHARED_DEFERRED_INIT_VECTOR_HPP