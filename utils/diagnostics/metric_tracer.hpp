// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_METRIC_TRACER_HPP
#define WFX_UTILS_METRIC_TRACER_HPP

// One 64-byte aligned block per worker in a single shared mmap
// Workers write only their own block, no locks or atomics needed
//
// Each block is laid out as:
//   [ WorkerMetrics ][ RouteMetrics x maxRoutes ][ EndpointMetrics x maxEndpoints ]
//   [ LatencyMetrics x maxRoutes ][ LatencyMetrics x maxEndpoints ]   (last two only when latency is on)
//
// The route/endpoint arrays are sized from config at Create() time, before fork, so the count is a-
// -fixed ceiling rather than the actual number registered. Unused slots never fault in a page, so-
// -a roomy cap costs address space, not memory. Registration hard-fails past the cap, so an index-
// -is never out of range and a scrape never reads an uninitialized slot
//
// Usage:
//   Master (before fork):
//     MetricTracer::Create(workerCount, maxRoutes, maxEndpoints, latency)
//
//   Worker (right after fork):
//     MetricTracer::InitWorker(index)
//
//   Worker hot path:
//     if(auto* m = MetricTracer::Current())          m->network.accepts++;
//     if(auto* r = MetricTracer::CurrentRoute(idx))   r->requests++;
//
//   On scrape (any worker):
//     auto net = MetricTracer::AggregateNetwork();

#include "shared/abis/types.hpp"
#include <bit>
#include <cstddef>
#include <cstdint>

namespace WFX::Utils {
namespace MetricTracer {

// vvv Process-local state vvv
inline std::byte* GlobalBase = nullptr;
inline int GlobalWorkerCount = 0;
inline int GlobalWorkerIndex = -1;
inline std::size_t GlobalMmapSize = 0;

// vvv Block layout, computed once in Create() vvv
inline std::size_t GlobalStride = 0;            // Bytes per worker block, 64-aligned
inline std::size_t GlobalRouteOffset = 0;       // Byte offset of the route array within a block
inline std::size_t GlobalEndpointOffset = 0;    // Byte offset of the endpoint array within a block
inline std::size_t GlobalLatRouteOffset = 0;    // Route latency array, valid only when latency is on
inline std::size_t GlobalLatEndpointOffset = 0; // Endpoint latency array, valid only when latency is on
inline std::uint16_t GlobalMaxRoutes = 0;
inline std::uint16_t GlobalMaxEndpoints = 0;
inline bool GlobalLatencyEnabled = false;

// vvv Lifecycle vvv
// Call once in master before fork. maxRoutes/maxEndpoints are ceilings, latency toggles the two-
// -latency arrays on or off entirely
bool Create(int workerCount, std::uint16_t maxRoutes, std::uint16_t maxEndpoints, bool latency) noexcept;
void InitWorker(int index) noexcept; // Call in each worker right after fork
void Destroy() noexcept;             // Call in master on shutdown

// vvv Block addressing vvv
// Start of a worker's block, nullptr if out of range
inline std::byte* Block(int worker) noexcept
{
    if(!GlobalBase || worker < 0 || worker >= GlobalWorkerCount)
        return nullptr;

    return GlobalBase + static_cast<std::size_t>(worker) * GlobalStride;
}

// vvv WorkerMetrics accessors vvv
// Returns a specific worker's slot. nullptr if out of range
inline Shared::WorkerMetrics* Slot(int worker) noexcept
{
    std::byte* b = Block(worker);
    return b ? reinterpret_cast<Shared::WorkerMetrics*>(b) : nullptr;
}

// Returns this worker's own slot. nullptr if not initialized
inline Shared::WorkerMetrics* Current() noexcept
{
    return Slot(GlobalWorkerIndex);
}

// vvv Route / endpoint array bases for a given worker vvv
inline Shared::RouteMetrics* RouteSlots(int worker) noexcept
{
    std::byte* b = Block(worker);
    return b ? reinterpret_cast<Shared::RouteMetrics*>(b + GlobalRouteOffset) : nullptr;
}

inline Shared::EndpointMetrics* EndpointSlots(int worker) noexcept
{
    std::byte* b = Block(worker);
    return b ? reinterpret_cast<Shared::EndpointMetrics*>(b + GlobalEndpointOffset) : nullptr;
}

inline Shared::LatencyMetrics* RouteLatencySlots(int worker) noexcept
{
    if(!GlobalLatencyEnabled)
        return nullptr;

    std::byte* b = Block(worker);
    return b ? reinterpret_cast<Shared::LatencyMetrics*>(b + GlobalLatRouteOffset) : nullptr;
}

inline Shared::LatencyMetrics* EndpointLatencySlots(int worker) noexcept
{
    if(!GlobalLatencyEnabled)
        return nullptr;

    std::byte* b = Block(worker);
    return b ? reinterpret_cast<Shared::LatencyMetrics*>(b + GlobalLatEndpointOffset) : nullptr;
}

// vvv Hot-path helpers for this worker's own counters vvv
// The bounds check is a safety net: registration already hard-fails past the cap, so a live index-
// -is always in range
inline Shared::RouteMetrics* CurrentRoute(std::uint16_t routeIdx) noexcept
{
    if(routeIdx >= GlobalMaxRoutes)
        return nullptr;

    Shared::RouteMetrics* r = RouteSlots(GlobalWorkerIndex);
    return r ? &r[routeIdx] : nullptr;
}

inline Shared::EndpointMetrics* CurrentEndpoint(std::uint16_t endpointIdx) noexcept
{
    if(endpointIdx >= GlobalMaxEndpoints)
        return nullptr;

    Shared::EndpointMetrics* e = EndpointSlots(GlobalWorkerIndex);
    return e ? &e[endpointIdx] : nullptr;
}

inline Shared::LatencyMetrics* CurrentRouteLatency(std::uint16_t routeIdx) noexcept
{
    if(routeIdx >= GlobalMaxRoutes)
        return nullptr;

    Shared::LatencyMetrics* l = RouteLatencySlots(GlobalWorkerIndex);
    return l ? &l[routeIdx] : nullptr;
}

inline Shared::LatencyMetrics* CurrentEndpointLatency(std::uint16_t endpointIdx) noexcept
{
    if(endpointIdx >= GlobalMaxEndpoints)
        return nullptr;

    Shared::LatencyMetrics* l = EndpointLatencySlots(GlobalWorkerIndex);
    return l ? &l[endpointIdx] : nullptr;
}

// Maps a microsecond duration to its histogram bucket. Eight linear sub-buckets per power-of-two-
// -octave, so each bucket is within 12.5% of its own width and a sample dropped at the midpoint is-
// -within 6.25% of the truth. Durations past the top octave saturate the last bucket
inline std::uint32_t LatencyBucketIndex(std::uint64_t us) noexcept
{
    if(us == 0)
        return 0;

    const std::uint32_t k = 63u - static_cast<std::uint32_t>(std::countl_zero(us)); // floor(log2(us))
    const std::uint32_t sub = (k >= 3) ? static_cast<std::uint32_t>((us >> (k - 3)) & 0x7u)
                                       : static_cast<std::uint32_t>((us - (1ull << k)) << (3u - k));
    const std::uint32_t idx = k * 8u + sub;

    return idx < Shared::LATENCY_BUCKET_COUNT ? idx : Shared::LATENCY_BUCKET_COUNT - 1;
}

// Record one latency sample into this worker's route/endpoint slot. No-op when latency is off-
// -(the slot accessor returns null), so callers need not branch on it themselves
inline void RecordRouteLatencyUs(std::uint16_t routeIdx, std::uint64_t us) noexcept
{
    Shared::LatencyMetrics* l = CurrentRouteLatency(routeIdx);
    if(!l)
        return;

    l->sumUs += us;
    l->buckets[LatencyBucketIndex(us)]++;
}

inline void RecordEndpointLatencyUs(std::uint16_t endpointIdx, std::uint64_t us) noexcept
{
    Shared::LatencyMetrics* l = CurrentEndpointLatency(endpointIdx);
    if(!l)
        return;

    l->sumUs += us;
    l->buckets[LatencyBucketIndex(us)]++;
}

inline int WorkerCount() noexcept
{
    return GlobalWorkerCount;
}
inline int WorkerIndex() noexcept
{
    return GlobalWorkerIndex;
}
inline bool IsReady() noexcept
{
    return GlobalBase != nullptr;
}
inline bool LatencyEnabled() noexcept
{
    return GlobalLatencyEnabled;
}

// vvv Aggregation across workers vvv
Shared::LogMetrics AggregateLog() noexcept;
Shared::NetworkMetrics AggregateNetwork() noexcept;
Shared::SelfMetrics AggregateSelf() noexcept;

// Sum one route/endpoint slot across every worker. A latency aggregate is empty when latency is off
Shared::RouteMetrics AggregateRoute(std::uint16_t routeIdx) noexcept;
Shared::EndpointMetrics AggregateEndpoint(std::uint16_t endpointIdx) noexcept;
Shared::LatencyMetrics AggregateRouteLatency(std::uint16_t routeIdx) noexcept;
Shared::LatencyMetrics AggregateEndpointLatency(std::uint16_t endpointIdx) noexcept;

} // namespace MetricTracer
} // namespace WFX::Utils

#endif // WFX_UTILS_METRIC_TRACER_HPP
