// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "utils/diagnostics/metric_tracer.hpp"

#include <sys/mman.h>

namespace WFX::Utils {
namespace MetricTracer {

bool Create(int workerCount) noexcept
{
    if(workerCount <= 0)
        return false;

    const std::size_t size = static_cast<std::size_t>(workerCount) * sizeof(Shared::WorkerMetrics);

    void* mem = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if(mem == MAP_FAILED)
        return false;

    GlobalSlots = static_cast<Shared::WorkerMetrics*>(mem);
    GlobalWorkerCount = workerCount;
    GlobalMmapSize = size;

    return true;
}

void InitWorker(int index) noexcept
{
    if(GlobalSlots && index >= 0 && index < GlobalWorkerCount)
        GlobalWorkerIndex = index;
}

void Destroy() noexcept
{
    if(GlobalSlots) {
        ::munmap(GlobalSlots, GlobalMmapSize);
        GlobalSlots = nullptr;
        GlobalWorkerCount = 0;
        GlobalWorkerIndex = -1;
        GlobalMmapSize = 0;
    }
}

Shared::LogMetrics AggregateLog() noexcept
{
    Shared::LogMetrics out{};
    if(!GlobalSlots)
        return out;

    for(int i = 0; i < GlobalWorkerCount; ++i) {
        const Shared::LogMetrics& l = GlobalSlots[i].log;
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
    if(!GlobalSlots)
        return out;

    for(int i = 0; i < GlobalWorkerCount; ++i) {
        const Shared::NetworkMetrics& n = GlobalSlots[i].network;
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
    if(!GlobalSlots)
        return out;

    for(int i = 0; i < GlobalWorkerCount; ++i) {
        const Shared::SelfMetrics& s = GlobalSlots[i].self;
        out.rssBytes += s.rssBytes;
        out.vmBytes += s.vmBytes;
        out.restarts += s.restarts;
        out.crashes += s.crashes;
    }

    return out;
}

} // namespace MetricTracer
} // namespace WFX::Utils