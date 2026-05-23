#ifndef WFX_SHARED_UTILS_API_HPP
#define WFX_SHARED_UTILS_API_HPP

#include "shared/abis/types.hpp"

namespace WFX::Shared {

// vvv All aliases for clarity vvv
using LogFn = void(*)(const char* msg);

using PrometheusFlushFn = void(*)(void* backend);

// vvv API declarations vvv
struct UTILS_API_EXT1 {
    // Logging
    LogFn LogTrace;
    LogFn LogDebug;
    LogFn LogInfo;
    LogFn LogWarn;
    LogFn LogError;
    LogFn LogFatal;

    // Prometheus
    PrometheusFlushFn PrometheusFlush;
};
static_assert(std::is_standard_layout<UTILS_API_EXT1>::value, "'UTILS_API_EXT1' must be standard layout");

// vvv Getter vvv
const UTILS_API_EXT1* GetUtilsAPIExt1();

} // namespace WFX::Shared

#endif // WFX_SHARED_UTILS_API_HPP