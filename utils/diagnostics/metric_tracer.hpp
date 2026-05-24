#ifndef WFX_UTILS_METRIC_TRACER_HPP
#define WFX_UTILS_METRIC_TRACER_HPP

// mmap(MAP_SHARED|MAP_ANONYMOUS) before fork: inherited by all children
// One cache line isolated 'WorkerMetrics' slot per worker
// Workers write only their own slot, no locks or atomics needed
//
// 'WorkerMetrics' embeds 'LogMetrics' and 'NetworkMetrics' directly
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
#include <cstdint>
#include <cstddef>

#ifdef _WIN32
    // Windows: future work
#else
    #include <sys/mman.h>
#endif

namespace WFX::Utils {

class MetricTracer {
public:
    // Call once in master before fork
    static bool Create(int workerCount) noexcept
    {
        if(workerCount <= 0)
            return false;

#ifdef _WIN32
        return false; // Windows: future work
#else
        const std::size_t size = static_cast<std::size_t>(workerCount) * sizeof(Shared::WorkerMetrics);

        void* mem = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if(mem == MAP_FAILED)
            return false;

        slots_       = static_cast<Shared::WorkerMetrics*>(mem);
        workerCount_ = workerCount;
        mmapSize_    = size;

        return true;
#endif
    }

    // Call in each worker right after fork to register its slot index
    static void InitWorker(int index) noexcept
    {
        if(slots_ && index >= 0 && index < workerCount_)
            workerIndex_ = index;
    }

    static void Destroy() noexcept
    {
#ifndef _WIN32
        if(slots_) {
            ::munmap(slots_, mmapSize_);
            slots_       = nullptr;
            workerCount_ = 0;
            workerIndex_ = -1;
            mmapSize_    = 0;
        }
#endif
    }

    // Returns pointer to this worker's own slot. nullptr if not initialized
    static Shared::WorkerMetrics* Current() noexcept
    {
        if(!slots_ || workerIndex_ < 0)
            return nullptr;

        return &slots_[workerIndex_];
    }

    // Direct read of a specific worker slot (for per-worker dashboards)
    static const Shared::WorkerMetrics* Slot(int index) noexcept
    {
        if(!slots_ || index < 0 || index >= workerCount_)
            return nullptr;

        return &slots_[index];
    }

    // Aggregate log metrics across all workers
    static Shared::LogMetrics AggregateLog() noexcept
    {
        Shared::LogMetrics out{};
        if(!slots_)
            return out;

        for(int i = 0; i < workerCount_; ++i) {
            const Shared::LogMetrics& l = slots_[i].log;
            out.trace += l.trace;
            out.debug += l.debug;
            out.info  += l.info;
            out.warn  += l.warn;
            out.error += l.error;
            out.fatal += l.fatal;
        }

        return out;
    }

    // Aggregate network metrics across all workers
    static Shared::NetworkMetrics AggregateNetwork() noexcept
    {
        Shared::NetworkMetrics out{};
        if(!slots_)
            return out;

        for(int i = 0; i < workerCount_; ++i) {
            const Shared::NetworkMetrics& n = slots_[i].network;
            out.accepts      += n.accepts;
            out.reads        += n.reads;
            out.writes       += n.writes;
            out.bytesRead    += n.bytesRead;
            out.bytesWritten += n.bytesWritten;
            out.activeConns  += n.activeConns;
            out.requests     += n.requests;
            out.response1xx  += n.response1xx;
            out.response2xx  += n.response2xx;
            out.response3xx  += n.response3xx;
            out.response4xx  += n.response4xx;
            out.response5xx  += n.response5xx;
        }

        return out;
    }

    static int  WorkerCount() noexcept { return workerCount_; }
    static int  WorkerIndex() noexcept { return workerIndex_; }
    static bool IsReady()     noexcept { return slots_ != nullptr; }

private:
    MetricTracer()  = delete;
    ~MetricTracer() = delete;

    static Shared::WorkerMetrics* slots_;       // points into shared mmap
    static int                    workerCount_; // process-local copy
    static int                    workerIndex_; // process-local, set after fork
    static std::size_t            mmapSize_;    // for munmap
};

inline Shared::WorkerMetrics* MetricTracer::slots_       = nullptr;
inline int                    MetricTracer::workerCount_ = 0;
inline int                    MetricTracer::workerIndex_ = -1;
inline std::size_t            MetricTracer::mmapSize_    = 0;

} // namespace WFX::Utils

#endif // WFX_UTILS_METRIC_TRACER_HPP