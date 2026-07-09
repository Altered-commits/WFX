// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "utils/diagnostics/metric_tracer.hpp"

#ifdef _WIN32
// Windows: future work
#else
#include <sys/mman.h>
#endif

namespace WFX::Utils {
namespace MetricTracer {

bool Create(int workerCount) noexcept
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

    slots_ = static_cast<Shared::WorkerMetrics*>(mem);
    workerCount_ = workerCount;
    mmapSize_ = size;

    return true;
#endif
}

void InitWorker(int index) noexcept
{
    if(slots_ && index >= 0 && index < workerCount_)
        workerIndex_ = index;
}

void Destroy() noexcept
{
#ifdef _WIN32
    // Windows: future work
#else
    if(slots_) {
        ::munmap(slots_, mmapSize_);
        slots_ = nullptr;
        workerCount_ = 0;
        workerIndex_ = -1;
        mmapSize_ = 0;
    }
#endif
}

Shared::LogMetrics AggregateLog() noexcept
{
    Shared::LogMetrics out{};
    if(!slots_)
        return out;

    for(int i = 0; i < workerCount_; ++i) {
        const Shared::LogMetrics& l = slots_[i].log;
        out.trace += l.trace;
        out.debug += l.debug;
        out.info += l.info;
        out.warn += l.warn;
        out.error += l.error;
        out.fatal += l.fatal;
    }

    return out;
}

Shared::NetworkMetrics AggregateNetwork() noexcept
{
    Shared::NetworkMetrics out{};
    if(!slots_)
        return out;

    for(int i = 0; i < workerCount_; ++i) {
        const Shared::NetworkMetrics& n = slots_[i].network;
        out.accepts += n.accepts;
        out.reads += n.reads;
        out.writes += n.writes;
        out.bytesRead += n.bytesRead;
        out.bytesWritten += n.bytesWritten;
        out.activeClientConns += n.activeClientConns;
        out.activeEndpointConns += n.activeEndpointConns;
        out.fileCalls += n.fileCalls;
        out.fileFallbacks += n.fileFallbacks;
        out.fileBytesWritten += n.fileBytesWritten;
        out.requests += n.requests;
        out.response1xx += n.response1xx;
        out.response2xx += n.response2xx;
        out.response3xx += n.response3xx;
        out.response4xx += n.response4xx;
        out.response5xx += n.response5xx;
    }

    return out;
}

Shared::SelfMetrics AggregateSelf() noexcept
{
    Shared::SelfMetrics out{};
    if(!slots_)
        return out;

    for(int i = 0; i < workerCount_; ++i) {
        const Shared::SelfMetrics& s = slots_[i].self;
        out.rssBytes += s.rssBytes;
        out.vmBytes += s.vmBytes;
        out.restarts += s.restarts;
        out.crashes += s.crashes;
    }

    return out;
}

} // namespace MetricTracer
} // namespace WFX::Utils