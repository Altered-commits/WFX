#ifndef WFX_UTILS_PERF_TIMER_HPP
#define WFX_UTILS_PERF_TIMER_HPP

#include <chrono>
#include <iostream>

#include "utils/logger/logger.hpp"

/*
 * NOTE: PURELY FOR DEBUGGING HOT PATHS IN CODE
 */

#define WFX_DEBUG 0

#if WFX_DEBUG

#define WFX_PROFILE_BLOCK_START(name) \
    auto __profile_start_##name = std::chrono::high_resolution_clock::now();

#define WFX_PROFILE_BLOCK_END(name) \
    do { \
        auto __profile_end_##name = std::chrono::high_resolution_clock::now(); \
        auto __profile_duration_##name = std::chrono::duration_cast<std::chrono::microseconds>(__profile_end_##name - __profile_start_##name).count(); \
        WFX::Utils::Logger::GetInstance().Info("[PROFILE][", #name, "]: ", __profile_duration_##name, " us");\
    } while(0);

#else

#define WFX_PROFILE_BLOCK_START(name)
#define WFX_PROFILE_BLOCK_END(name)

#endif // _DEBUG

#endif