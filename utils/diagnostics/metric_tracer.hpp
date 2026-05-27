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
inline Shared::WorkerMetrics* slots_ = nullptr;
inline int workerCount_ = 0;
inline int workerIndex_ = -1;
inline std::size_t mmapSize_ = 0;

// vvv Lifecycle vvv
bool Create(int workerCount) noexcept; // Call once in master before fork
void InitWorker(int index) noexcept;   // Call in each worker right after fork
void Destroy() noexcept;               // Call in master on shutdown

// vvv Accessors vvv
// Returns this worker's own slot. nullptr if not initialized
inline Shared::WorkerMetrics* Current() noexcept
{
    if(!slots_ || workerIndex_ < 0)
        return nullptr;

    return &slots_[workerIndex_];
}

// Returns a specific worker's slot. nullptr if out of range
inline Shared::WorkerMetrics* Slot(int index) noexcept
{
    if(!slots_ || index < 0 || index >= workerCount_)
        return nullptr;

    return &slots_[index];
}

inline int WorkerCount() noexcept
{
    return workerCount_;
}
inline int WorkerIndex() noexcept
{
    return workerIndex_;
}
inline bool IsReady() noexcept
{
    return slots_ != nullptr;
}

// vvv Aggregation vvv
Shared::LogMetrics AggregateLog() noexcept;
Shared::NetworkMetrics AggregateNetwork() noexcept;
Shared::SelfMetrics AggregateSelf() noexcept;

} // namespace MetricTracer
} // namespace WFX::Utils

#endif // WFX_UTILS_METRIC_TRACER_HPP