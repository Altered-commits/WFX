#ifndef WFX_UTILS_SHARED_METRICS_HPP
#define WFX_UTILS_SHARED_METRICS_HPP

// mmap(MAP_SHARED|MAP_ANONYMOUS) before fork: inherited by all children
// One cache-line-isolated slot per worker indexed by worker index
// Workers write only their own slot, no locks or atomics needed
//
// Usage:
//   Master (before fork):
//     SharedMetrics::Create(workerCount)
//
//   Worker (right after fork):
//     SharedMetrics::InitWorker(index)
//
//   Worker hot path (any subsystem):
//     if(auto* m = SharedMetrics::Current()) m->accepts++;
//
//   On /metrics scrape (any worker):
//     auto agg = SharedMetrics::Aggregate();

#include <cstdint>
#include <cstddef>

#ifdef _WIN32
    // Windows: future work
#else
    #include <sys/mman.h>
#endif

namespace WFX::Utils {

// One slot per worker. Caller writes fields directly.
// alignas(64) + sizeof % 64 == 0 prevents false sharing between adjacent slots
struct alignas(64) WorkerMetrics {
    // Logger (indexed by Logger::Level -> 0=trace .. 5=fatal)
    std::uint64_t logLines[6] = {};

    // Network
    std::uint64_t accepts      = 0;
    std::uint64_t reads        = 0;
    std::uint64_t writes       = 0;
    std::uint64_t bytesRead    = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t activeConns  = 0;

    // Requests
    std::uint64_t requests    = 0;
    std::uint64_t requests2xx = 0;
    std::uint64_t requests4xx = 0;
    std::uint64_t requests5xx = 0;
};

static_assert(sizeof(WorkerMetrics) == 128,    "WorkerMetrics must be exactly 128 bytes");
static_assert(sizeof(WorkerMetrics) % 64 == 0, "WorkerMetrics must be a multiple of 64 bytes");

// Summed view across all workers
struct AggregatedMetrics {
    std::uint64_t logLines[6] = {};
    std::uint64_t accepts      = 0;
    std::uint64_t reads        = 0;
    std::uint64_t writes       = 0;
    std::uint64_t bytesRead    = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t activeConns  = 0;
    std::uint64_t requests     = 0;
    std::uint64_t requests2xx  = 0;
    std::uint64_t requests4xx  = 0;
    std::uint64_t requests5xx  = 0;
};

class SharedMetrics {
public:
    // Call once in master before fork
    static bool Create(int workerCount) noexcept
    {
        if(workerCount <= 0)
            return false;

#ifdef _WIN32
        return false; // Windows: future work
#else
        const std::size_t size = static_cast<std::size_t>(workerCount) * sizeof(WorkerMetrics);

        void* mem = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if(mem == MAP_FAILED)
            return false;

        slots_       = static_cast<WorkerMetrics*>(mem);
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
    static WorkerMetrics* Current() noexcept
    {
        if(!slots_ || workerIndex_ < 0)
            return nullptr;

        return &slots_[workerIndex_];
    }

    // Aggregate all worker slots
    static AggregatedMetrics Aggregate() noexcept
    {
        AggregatedMetrics out{};
        if(!slots_)
            return out;

        for(int i = 0; i < workerCount_; ++i) {
            const WorkerMetrics& w = slots_[i];

            for(int l = 0; l < 6; ++l)
                out.logLines[l] += w.logLines[l];

            out.accepts      += w.accepts;
            out.reads        += w.reads;
            out.writes       += w.writes;
            out.bytesRead    += w.bytesRead;
            out.bytesWritten += w.bytesWritten;
            out.activeConns  += w.activeConns;
            out.requests     += w.requests;
            out.requests2xx  += w.requests2xx;
            out.requests4xx  += w.requests4xx;
            out.requests5xx  += w.requests5xx;
        }

        return out;
    }

    // Direct read of a specific worker slot (for per-worker dashboards)
    static const WorkerMetrics* Slot(int index) noexcept
    {
        if(!slots_ || index < 0 || index >= workerCount_)
            return nullptr;

        return &slots_[index];
    }

    static int  WorkerCount() noexcept { return workerCount_; }
    static int  WorkerIndex() noexcept { return workerIndex_; }
    static bool IsReady()     noexcept { return slots_ != nullptr; }

private:
    SharedMetrics()  = delete;
    ~SharedMetrics() = delete;

    static WorkerMetrics* slots_;       // points into shared mmap
    static int            workerCount_; // process-local copy
    static int            workerIndex_; // process-local, set after fork
    static std::size_t    mmapSize_;    // for munmap
};

inline WorkerMetrics* SharedMetrics::slots_       = nullptr;
inline int            SharedMetrics::workerCount_ = 0;
inline int            SharedMetrics::workerIndex_ = -1;
inline std::size_t    SharedMetrics::mmapSize_     = 0;

} // namespace WFX::Utils

#endif // WFX_UTILS_SHARED_METRICS_HPP