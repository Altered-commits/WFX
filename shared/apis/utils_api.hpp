// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_UTILS_API_HPP
#define WFX_SHARED_UTILS_API_HPP

#include "shared/abis/types.hpp"

namespace WFX::Shared {

// vvv All aliases for clarity vvv
using LogFn = void (*)(const char* msg);

using GetLogMetricsWorkerFn = LogMetrics (*)();
using GetNetMetricsWorkerFn = NetworkMetrics (*)();
using GetSelfMetricsWorkerFn = SelfMetrics (*)();
using GetLogMetricsAggregateFn = LogMetrics (*)();
using GetNetMetricsAggregateFn = NetworkMetrics (*)();
using GetSelfMetricsAggregateFn = SelfMetrics (*)();

// vvv API declarations vvv
struct UTILS_API_EXT1 {
    // Logging
    LogFn LogTrace;
    LogFn LogDebug;
    LogFn LogInfo;
    LogFn LogWarn;
    LogFn LogError;
    LogFn LogFatal;

    // Metrics
    GetLogMetricsWorkerFn GetLogMetricsWorker;
    GetNetMetricsWorkerFn GetNetMetricsWorker;
    GetSelfMetricsWorkerFn GetSelfMetricsWorker;
    GetLogMetricsAggregateFn GetLogMetricsAggregate;
    GetNetMetricsAggregateFn GetNetMetricsAggregate;
    GetSelfMetricsAggregateFn GetSelfMetricsAggregate;
};
static_assert(std::is_standard_layout<UTILS_API_EXT1>::value, "'UTILS_API_EXT1' must be standard layout");

// vvv Getter vvv
const UTILS_API_EXT1* GetUtilsAPIExt1();

} // namespace WFX::Shared

#endif // WFX_SHARED_UTILS_API_HPP