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
struct UtilsAPIExt1 {
    // Logging
    LogFn logTrace;
    LogFn logDebug;
    LogFn logInfo;
    LogFn logWarn;
    LogFn logError;
    LogFn logFatal;

    // Metrics
    GetLogMetricsWorkerFn getLogMetricsWorker;
    GetNetMetricsWorkerFn getNetMetricsWorker;
    GetSelfMetricsWorkerFn getSelfMetricsWorker;
    GetLogMetricsAggregateFn getLogMetricsAggregate;
    GetNetMetricsAggregateFn getNetMetricsAggregate;
    GetSelfMetricsAggregateFn getSelfMetricsAggregate;
};
static_assert(std::is_standard_layout<UtilsAPIExt1>::value, "'UTILS_API_EXT1' must be standard layout");

// vvv Getter vvv
const UtilsAPIExt1* GetUtilsAPIExt1();

} // namespace WFX::Shared

#endif // WFX_SHARED_UTILS_API_HPP