#ifndef WFX_SHARED_UTILS_API_HPP
#define WFX_SHARED_UTILS_API_HPP

#include "shared/abis/types.hpp"

namespace WFX::Shared {

enum class UtilsAPIVersion : std::uint8_t {
    V1 = 1,
};

// vvv All aliases for clarity vvv
using LogInfoFn  = void(*)(const char* msg);
using LogWarnFn  = void(*)(const char* msg);
using LogErrorFn = void(*)(const char* msg);
using LogFatalFn = void(*)(const char* msg);

// vvv API declarations vvv
struct UTILS_API_TABLE {
    // vvv Logging Operations vvv
    LogInfoFn   LogInfo;
    LogWarnFn   LogWarn;
    LogErrorFn  LogError;
    LogFatalFn  LogFatal;

    // Metadata
    UtilsAPIVersion apiVersion;
};
static_assert(std::is_standard_layout<UTILS_API_TABLE>::value, "'UTILS_API_TABLE' must be standard layout");

// vvv Getter & Initializers vvv
const UTILS_API_TABLE* GetUtilsAPIV1();

} // namespace WFX::Shared

#endif // WFX_SHARED_UTILS_API_HPP