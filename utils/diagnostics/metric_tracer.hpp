// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_METRIC_TRACER_HPP
#define WFX_UTILS_METRIC_TRACER_HPP

// One cache line isolated 'WorkerMetrics' slot per worker
// Workers write only their own slot, no locks or atomics needed
//
// 'WorkerMetrics' embeds 'LogMetrics', 'NetworkMetrics' and 'SelfMetrics' directly
//
// Usage:
//   Master (before fork):
//     MetricTracer::Create(workerCount)
//
//   Worker (right after fork):
//     MetricTracer::InitWorker(index)
//
//   Worker hot path:
//     if(auto* m = MetricTracer::Current()) m->log.info++;
//     if(auto* m = MetricTracer::Current()) m->network.accepts++;
//
//   On /metrics scrape (any worker):
//     auto log = MetricTracer::AggregateLog();
//     auto net = MetricTracer::AggregateNetwork();

#include "shared/abis/types.hpp"
#include <cstddef>
#include <cstdint>

namespace WFX::Utils {
namespace MetricTracer {

// vvv Process-local state vvv
inline Shared::WorkerMetrics* GlobalSlots = nullptr;
inline int GlobalWorkerCount = 0;
inline int GlobalWorkerIndex = -1;
inline std::size_t GlobalMmapSize = 0;

// vvv Lifecycle vvv
bool Create(int workerCount) noexcept; // Call once in master before fork
void InitWorker(int index) noexcept;   // Call in each worker right after fork
void Destroy() noexcept;               // Call in master on shutdown

// vvv Accessors vvv
// Returns this worker's own slot. nullptr if not initialized
inline Shared::WorkerMetrics* Current() noexcept
{
    if(!GlobalSlots || GlobalWorkerIndex < 0)
        return nullptr;

    return &GlobalSlots[GlobalWorkerIndex];
}

// Returns a specific worker's slot. nullptr if out of range
inline Shared::WorkerMetrics* Slot(int index) noexcept
{
    if(!GlobalSlots || index < 0 || index >= GlobalWorkerCount)
        return nullptr;

    return &GlobalSlots[index];
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
    return GlobalSlots != nullptr;
}

// vvv Aggregation vvv
Shared::LogMetrics AggregateLog() noexcept;
Shared::NetworkMetrics AggregateNetwork() noexcept;
Shared::SelfMetrics AggregateSelf() noexcept;

} // namespace MetricTracer
} // namespace WFX::Utils

#endif // WFX_UTILS_METRIC_TRACER_HPP