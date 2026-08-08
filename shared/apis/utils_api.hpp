// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_UTILS_API_HPP
#define WFX_SHARED_UTILS_API_HPP

#include "shared/abis/types.hpp"

// Identity for route/endpoint metrics is read back from these at scrape time
namespace WFX::Http {
class Router;
struct HttpConnectionHandler;
} // namespace WFX::Http

namespace WFX::Shared {

// vvv All aliases for clarity vvv
using LogFn = void (*)(const char* msg);

// Worker slots are traversed by index, so a caller needs the count and, to find itself, its own
// index. Everything per-worker is reachable from those two.
using GetWorkerCountFn = std::uint16_t (*)();
using GetWorkerIndexFn = std::uint16_t (*)();

using GetLogMetricsAtFn = LogMetrics (*)(std::uint16_t worker);
using GetNetMetricsAtFn = NetworkMetrics (*)(std::uint16_t worker);
using GetSelfMetricsAtFn = SelfMetrics (*)(std::uint16_t worker);

using GetLogMetricsAggregateFn = LogMetrics (*)();
using GetNetMetricsAggregateFn = NetworkMetrics (*)();
using GetSelfMetricsAggregateFn = SelfMetrics (*)();

// Route and endpoint metrics are aggregated across workers and returned with their identity
// attached. Latency lives in its own array, mapped only when [Metrics] latency is on, so it has
// its own getters and a flag to tell whether the buckets mean anything.
using GetRouteMetricCountFn = std::uint16_t (*)();
using GetEndpointMetricCountFn = std::uint16_t (*)();
using GetRouteMetricsAtFn = RouteMetricsView (*)(std::uint16_t routeIdx);
using GetEndpointMetricsAtFn = EndpointMetricsView (*)(std::uint16_t endpointIdx);
using GetRouteLatencyAtFn = LatencyMetrics (*)(std::uint16_t routeIdx);
using GetEndpointLatencyAtFn = LatencyMetrics (*)(std::uint16_t endpointIdx);
using GetMetricsLatencyEnabledFn = bool (*)();

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
    GetWorkerCountFn getWorkerCount;
    GetWorkerIndexFn getWorkerIndex;
    GetLogMetricsAtFn getLogMetricsAt;
    GetNetMetricsAtFn getNetMetricsAt;
    GetSelfMetricsAtFn getSelfMetricsAt;
    GetLogMetricsAggregateFn getLogMetricsAggregate;
    GetNetMetricsAggregateFn getNetMetricsAggregate;
    GetSelfMetricsAggregateFn getSelfMetricsAggregate;
    GetRouteMetricCountFn getRouteMetricCount;
    GetEndpointMetricCountFn getEndpointMetricCount;
    GetRouteMetricsAtFn getRouteMetricsAt;
    GetEndpointMetricsAtFn getEndpointMetricsAt;
    GetRouteLatencyAtFn getRouteLatencyAt;
    GetEndpointLatencyAtFn getEndpointLatencyAt;
    GetMetricsLatencyEnabledFn getMetricsLatencyEnabled;
};
static_assert(std::is_standard_layout<UtilsAPIExt1>::value, "'UTILS_API_EXT1' must be standard layout");

// vvv Getter vvv
const UtilsAPIExt1* GetUtilsAPIExt1();

// Injects the registration data the route/endpoint metric getters read identity from. Called once
// per worker at startup, same as the http and endpoint API tables.
void InitUtilsAPIExt1(Http::Router* router, Http::HttpConnectionHandler* connHandler);

} // namespace WFX::Shared

#endif // WFX_SHARED_UTILS_API_HPP