// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_TELEMETRY_HPP
#define WFX_INC_WFX_TELEMETRY_HPP

// -----------------------------------------------------------------------
// wfx/telemetry.hpp
// Zero-alloc logging and metrics for user code.
//
// Logging crosses the ABI boundary as a single null-terminated string.
// Metrics live in a shared mmap, one slot per worker, and are returned as
// plain structs. Per-worker slots are reached by index, aggregates sum them
//
// Provides:
//   WFX::LogTrace  / WFX::LogDebug  / WFX::LogInfo
//   WFX::LogWarn   / WFX::LogError  / WFX::LogFatal
//
//   WFX::WorkerMetricCount()      : number of worker slots
//   WFX::WorkerIndex()            : index of the worker running this handler
//   WFX::GetLogMetricsAt(w)       : one worker's log counters
//   WFX::GetNetworkMetricsAt(w)   : one worker's network counters
//   WFX::GetProcessMetricsAt(w)   : one worker's process info
//   WFX::GetLogMetricsAll()       : aggregated across all workers
//   WFX::GetNetworkMetricsAll()   : aggregated across all workers
//   WFX::GetProcessMetricsAll()   : aggregated across all workers
//
//   WFX::RouteMetricCount()       : number of registered routes
//   WFX::EndpointMetricCount()    : number of registered endpoints
//   WFX::GetRouteMetricsAt(r)     : one route's counters + path/method, summed across workers
//   WFX::GetEndpointMetricsAt(e)  : one endpoint's counters + host, summed across workers
//   WFX::MetricsLatencyEnabled()  : whether [Metrics] latency is on
//   WFX::GetRouteLatencyAt(r)     : one route's latency histogram, summed across workers
//   WFX::GetEndpointLatencyAt(e)  : one endpoint's latency histogram, summed across workers
//   WFX::ComputeLatencyStats(h)   : histogram to count/mean/stddev/min/max/p50/p90/p95/p99/p999
//
// Usage:
//   WFX::LogInfo("[MyHandler]: started on port ", port);
//   WFX::LogWarn("[Auth]: token expiring in ", seconds, "s");
//   WFX::LogError("[DB]: query failed, code=", code);
//   WFX::LogFatal("[Init]: cannot continue");  // aborts
//
//   auto net = WFX::GetNetworkMetricsAll();
//   WFX::LogInfo("total accepts: ", net.accepts);
//
//   // This worker's own numbers
//   auto self = WFX::GetProcessMetricsAt(WFX::WorkerIndex());
//
//   // Per worker, so one sick worker stays visible instead of averaging away
//   for(std::uint16_t w = 0; w < WFX::WorkerMetricCount(); w++)
//       WFX::LogInfo("worker ", w, " rss=", WFX::GetProcessMetricsAt(w).rssBytes);
// -----------------------------------------------------------------------

#include "core/core.hpp"
#include "http/response.hpp"
#include "shared/utils/compiler_macro.hpp"
#include <charconv>
#include <cmath>
#include <type_traits>

namespace WFX {

namespace Detail {

// -----------------------------------------------------------------------
// Stack buffer size for formatted log lines
// Matches engine-side 'K_MSG_BUF_SIZE' (lines longer than this are truncated)
// -----------------------------------------------------------------------
static constexpr std::size_t kLogBufSize = 1024;

// vvv Fmt overloads vvv
inline char* LogFmt(char* p, char* end, std::string_view sv) noexcept
{
    const std::size_t n = std::min(static_cast<std::size_t>(end - p), sv.size());
    std::memcpy(p, sv.data(), n);
    return p + n;
}

inline char* LogFmt(char* p, char* end, const Shared::StringView& sv) noexcept
{
    return LogFmt(p, end, std::string_view(sv.Data(), sv.Size()));
}

inline char* LogFmt(char* p, char* end, const char* s) noexcept
{
    return LogFmt(p, end, std::string_view(s ? s : "(null)"));
}

inline char* LogFmt(char* p, char* end, char c) noexcept
{
    if(p < end)
        *p++ = c;
    return p;
}

inline char* LogFmt(char* p, char* end, bool v) noexcept
{
    return LogFmt(p, end, v ? "true" : "false");
}

template <typename T>
inline std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>, char*> LogFmt(
    char* p, char* end, T value) noexcept
{
    if(end - p < 24)
        return p;

    auto [ep, ec] = std::to_chars(p, p + 24, value);
    return ec == std::errc() ? ep : p;
}

template <typename T>
inline std::enable_if_t<std::is_floating_point_v<T>, char*> LogFmt(char* p, char* end, T value) noexcept
{
    if(end - p < 32)
        return p;

    auto [ep, ec] = std::to_chars(p, p + 32, value);
    return ec == std::errc() ? ep : p;
}

template <typename T>
inline std::enable_if_t<std::is_pointer_v<T> && !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>,
                        char*>
LogFmt(char* p, char* end, T value) noexcept
{
    if(end - p < 20)
        return p;

    *p++ = '0';
    *p++ = 'x';

    auto [ep, ec] = std::to_chars(p, p + 16, reinterpret_cast<std::uintptr_t>(value), 16);
    return ec == std::errc() ? ep : p;
}

// -----------------------------------------------------------------------
// Formats args into a stack buffer, crosses ABI boundary once via a
// single (const char*) call. 'lvl' maps directly to LogFn index
// -----------------------------------------------------------------------
template <typename... Args> inline void LogEmit(int lvl, Args&&... args) noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return;

    char buf[kLogBufSize];
    char* p = buf;
    char* end = p + kLogBufSize - 1;

    ((p = LogFmt(p, end, std::forward<Args>(args))), ...);

    // Signal truncation if we hit the limit
    if(p >= end - 1) {
        buf[kLogBufSize - 5] = '.';
        buf[kLogBufSize - 4] = '.';
        buf[kLogBufSize - 3] = '.';
        buf[kLogBufSize - 2] = '\0';
    }
    else
        *p = '\0';

    switch(lvl) {
        case 0:
            api->logTrace(buf);
            break;
        case 1:
            api->logDebug(buf);
            break;
        case 2:
            api->logInfo(buf);
            break;
        case 3:
            api->logWarn(buf);
            break;
        case 4:
            api->logError(buf);
            break;
        case 5:
            api->logFatal(buf);
            break;
    }
}

} // namespace Detail

// vvv Log API vvv
template <typename... Args> inline void LogTrace(Args&&... args) noexcept
{
    Detail::LogEmit(0, std::forward<Args>(args)...);
}

template <typename... Args> inline void LogDebug(Args&&... args) noexcept
{
    Detail::LogEmit(1, std::forward<Args>(args)...);
}

template <typename... Args> inline void LogInfo(Args&&... args) noexcept
{
    Detail::LogEmit(2, std::forward<Args>(args)...);
}

template <typename... Args> inline void LogWarn(Args&&... args) noexcept
{
    Detail::LogEmit(3, std::forward<Args>(args)...);
}

template <typename... Args> inline void LogError(Args&&... args) noexcept
{
    Detail::LogEmit(4, std::forward<Args>(args)...);
}

template <typename... Args> [[noreturn]] inline void LogFatal(Args&&... args) noexcept
{
    Detail::LogEmit(5, std::forward<Args>(args)...);
    WFX_UNREACHABLE;
}

// vvv Metrics API vvv
// Number of worker slots, and the index of the worker running this handler
// Together they cover both traversal and "what are my own numbers"
inline std::uint16_t WorkerMetricCount() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return 0;

    return api->getWorkerCount();
}

inline std::uint16_t WorkerIndex() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return 0;

    return api->getWorkerIndex();
}

// Out of range returns a zeroed struct rather than failing
inline Shared::LogMetrics GetLogMetricsAt(std::uint16_t worker) noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->getLogMetricsAt(worker);
}

inline Shared::NetworkMetrics GetNetworkMetricsAt(std::uint16_t worker) noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->getNetMetricsAt(worker);
}

inline Shared::SelfMetrics GetProcessMetricsAt(std::uint16_t worker) noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->getSelfMetricsAt(worker);
}

inline Shared::LogMetrics GetLogMetricsAll() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->getLogMetricsAggregate();
}

inline Shared::NetworkMetrics GetNetworkMetricsAll() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->getNetMetricsAggregate();
}

inline Shared::SelfMetrics GetProcessMetricsAll() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->getSelfMetricsAggregate();
}

// vvv Route / Endpoint metrics vvv
// Routes and endpoints are indexed densely from 0. Counts come from registration, so they are
// stable for the process, and each getter sums that slot across every worker with its identity
// (route path/method, endpoint host) attached. An out of range index returns a zeroed view
inline std::uint16_t RouteMetricCount() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return 0;

    return api->getRouteMetricCount();
}

inline std::uint16_t EndpointMetricCount() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return 0;

    return api->getEndpointMetricCount();
}

inline Shared::RouteMetricsView GetRouteMetricsAt(std::uint16_t route) noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->getRouteMetricsAt(route);
}

inline Shared::EndpointMetricsView GetEndpointMetricsAt(std::uint16_t endpoint) noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->getEndpointMetricsAt(endpoint);
}

// Latency histograms live in their own array, mapped only when [Metrics] latency is on. Check
// MetricsLatencyEnabled() first, the buckets are all zero when it is off
inline bool MetricsLatencyEnabled() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return false;

    return api->getMetricsLatencyEnabled();
}

inline Shared::LatencyMetrics GetRouteLatencyAt(std::uint16_t route) noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->getRouteLatencyAt(route);
}

inline Shared::LatencyMetrics GetEndpointLatencyAt(std::uint16_t endpoint) noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->getEndpointLatencyAt(endpoint);
}

// Derived latency numbers, everything you would actually alert on. All in microseconds. count and
// mean are exact (mean = sumUs / count); the percentiles, min, max and stddev are read off the
// bucket midpoints, so they carry the histogram's own <=6.25% error. stddev is midpoint-derived on
// purpose: an exact one needs a stored sum of squares, which overflows a 64-bit accumulator at
// production request volumes
struct LatencyStats {
    std::uint64_t count = 0;
    double meanUs = 0.0;
    double stddevUs = 0.0;
    std::uint64_t minUs = 0;
    std::uint64_t maxUs = 0;
    std::uint64_t p50Us = 0;
    std::uint64_t p90Us = 0;
    std::uint64_t p95Us = 0;
    std::uint64_t p99Us = 0;
    std::uint64_t p999Us = 0;
};

namespace Detail {

// Representative value of a bucket: the midpoint of the octave sub-range it covers. Bucket i sits
// in octave k = i / 8 (range [2^k, 2^(k+1))) at sub-bucket s = i % 8 of width 2^k / 8
inline std::uint64_t LatencyBucketMidpointUs(std::uint32_t idx) noexcept
{
    const std::uint32_t k = idx / 8;
    const std::uint32_t s = idx % 8;
    const double base = static_cast<double>(std::uint64_t{1} << k);

    return static_cast<std::uint64_t>(base + (static_cast<double>(s) + 0.5) * base / 8.0);
}

// Nearest-rank percentile over the cumulative bucket counts
inline std::uint64_t LatencyPercentile(const Shared::LatencyMetrics& h, std::uint64_t count, double q) noexcept
{
    if(count == 0)
        return 0;

    std::uint64_t rank = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count)));
    if(rank == 0)
        rank = 1;

    std::uint64_t cum = 0;
    for(std::uint32_t i = 0; i < Shared::LATENCY_BUCKET_COUNT; i++) {
        cum += h.buckets[i];
        if(cum >= rank)
            return LatencyBucketMidpointUs(i);
    }

    return LatencyBucketMidpointUs(Shared::LATENCY_BUCKET_COUNT - 1);
}

} // namespace Detail

// Turns a raw histogram (from GetRouteLatencyAt / GetEndpointLatencyAt) into the derived numbers.
// Returns a zeroed struct when no samples were recorded (latency off, or no traffic yet)
inline LatencyStats ComputeLatencyStats(const Shared::LatencyMetrics& h) noexcept
{
    LatencyStats stats{};

    std::uint64_t count = 0;
    for(std::uint32_t i = 0; i < Shared::LATENCY_BUCKET_COUNT; i++)
        count += h.buckets[i];

    stats.count = count;
    if(count == 0)
        return stats;

    stats.meanUs = static_cast<double>(h.sumUs) / static_cast<double>(count);

    // First and last non-empty buckets bound the observed range
    std::uint32_t firstIdx = 0;
    std::uint32_t lastIdx = 0;
    bool haveFirst = false;
    double variance = 0.0;

    for(std::uint32_t i = 0; i < Shared::LATENCY_BUCKET_COUNT; i++) {
        if(h.buckets[i] == 0)
            continue;

        if(!haveFirst) {
            firstIdx = i;
            haveFirst = true;
        }

        lastIdx = i;

        const double mid = static_cast<double>(Detail::LatencyBucketMidpointUs(i));
        const double diff = mid - stats.meanUs;
        variance += static_cast<double>(h.buckets[i]) * diff * diff;
    }

    stats.stddevUs = std::sqrt(variance / static_cast<double>(count));
    stats.minUs = Detail::LatencyBucketMidpointUs(firstIdx);
    stats.maxUs = Detail::LatencyBucketMidpointUs(lastIdx);
    stats.p50Us = Detail::LatencyPercentile(h, count, 0.50);
    stats.p90Us = Detail::LatencyPercentile(h, count, 0.90);
    stats.p95Us = Detail::LatencyPercentile(h, count, 0.95);
    stats.p99Us = Detail::LatencyPercentile(h, count, 0.99);
    stats.p999Us = Detail::LatencyPercentile(h, count, 0.999);

    return stats;
}

} // namespace WFX

#endif // WFX_INC_WFX_TELEMETRY_HPP