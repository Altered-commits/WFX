// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "utils/diagnostics/metric_tracer.hpp"

#include <sys/mman.h>

namespace WFX::Utils {
namespace MetricTracer {

// Rounds up to the next 64-byte boundary so each array, and each worker block, starts on its own
// cache line and no two workers ever share one.
static std::size_t Align64(std::size_t n) noexcept
{
    return (n + 63) & ~static_cast<std::size_t>(63);
}

bool Create(int workerCount, std::uint16_t maxRoutes, std::uint16_t maxEndpoints, bool latency) noexcept
{
    if(workerCount <= 0)
        return false;

    // sizeof(WorkerMetrics) is a multiple of 64, so the route array starts 64-aligned. Every array
    // after it is placed on the next 64-byte boundary, which also fixes the block stride.
    std::size_t offset = sizeof(Shared::WorkerMetrics);

    GlobalRouteOffset = offset;
    offset += Align64(static_cast<std::size_t>(maxRoutes) * sizeof(Shared::RouteMetrics));

    GlobalEndpointOffset = offset;
    offset += Align64(static_cast<std::size_t>(maxEndpoints) * sizeof(Shared::EndpointMetrics));

    if(latency) {
        GlobalLatRouteOffset = offset;
        offset += Align64(static_cast<std::size_t>(maxRoutes) * sizeof(Shared::LatencyMetrics));

        GlobalLatEndpointOffset = offset;
        offset += Align64(static_cast<std::size_t>(maxEndpoints) * sizeof(Shared::LatencyMetrics));
    }

    const std::size_t stride = Align64(offset);
    const std::size_t size = static_cast<std::size_t>(workerCount) * stride;

    void* mem = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if(mem == MAP_FAILED)
        return false;

    GlobalBase = static_cast<std::byte*>(mem);
    GlobalWorkerCount = workerCount;
    GlobalMmapSize = size;
    GlobalStride = stride;
    GlobalMaxRoutes = maxRoutes;
    GlobalMaxEndpoints = maxEndpoints;
    GlobalLatencyEnabled = latency;

    return true;
}

void InitWorker(int index) noexcept
{
    if(GlobalBase && index >= 0 && index < GlobalWorkerCount)
        GlobalWorkerIndex = index;
}

void Destroy() noexcept
{
    if(GlobalBase) {
        ::munmap(GlobalBase, GlobalMmapSize);
        GlobalBase = nullptr;
        GlobalWorkerCount = 0;
        GlobalWorkerIndex = -1;
        GlobalMmapSize = 0;
        GlobalStride = 0;
        GlobalMaxRoutes = 0;
        GlobalMaxEndpoints = 0;
        GlobalLatencyEnabled = false;
    }
}

Shared::LogMetrics AggregateLog() noexcept
{
    Shared::LogMetrics out{};

    for(int i = 0; i < GlobalWorkerCount; ++i) {
        const Shared::WorkerMetrics* m = Slot(i);
        if(!m)
            continue;

        const Shared::LogMetrics& l = m->log;
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

    for(int i = 0; i < GlobalWorkerCount; ++i) {
        const Shared::WorkerMetrics* m = Slot(i);
        if(!m)
            continue;

        const Shared::NetworkMetrics& n = m->network;
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
    }

    return out;
}

Shared::SelfMetrics AggregateSelf() noexcept
{
    Shared::SelfMetrics out{};

    for(int i = 0; i < GlobalWorkerCount; ++i) {
        const Shared::WorkerMetrics* m = Slot(i);
        if(!m)
            continue;

        const Shared::SelfMetrics& s = m->self;
        out.rssBytes += s.rssBytes;
        out.vmBytes += s.vmBytes;
        out.restarts += s.restarts;
        out.crashes += s.crashes;
    }

    return out;
}

Shared::RouteMetrics AggregateRoute(std::uint16_t routeIdx) noexcept
{
    Shared::RouteMetrics out{};
    if(routeIdx >= GlobalMaxRoutes)
        return out;

    for(int i = 0; i < GlobalWorkerCount; ++i) {
        const Shared::RouteMetrics* arr = RouteSlots(i);
        if(!arr)
            continue;

        const Shared::RouteMetrics& r = arr[routeIdx];
        out.requests += r.requests;
        out.status1xx += r.status1xx;
        out.status2xx += r.status2xx;
        out.status3xx += r.status3xx;
        out.status4xx += r.status4xx;
        out.status5xx += r.status5xx;
        out.bytesOut += r.bytesOut;
    }

    return out;
}

Shared::EndpointMetrics AggregateEndpoint(std::uint16_t endpointIdx) noexcept
{
    Shared::EndpointMetrics out{};
    if(endpointIdx >= GlobalMaxEndpoints)
        return out;

    for(int i = 0; i < GlobalWorkerCount; ++i) {
        const Shared::EndpointMetrics* arr = EndpointSlots(i);
        if(!arr)
            continue;

        const Shared::EndpointMetrics& e = arr[endpointIdx];
        out.requests += e.requests;
        out.completed += e.completed;
        out.status1xx += e.status1xx;
        out.status2xx += e.status2xx;
        out.status3xx += e.status3xx;
        out.status4xx += e.status4xx;
        out.status5xx += e.status5xx;
        out.connectFailures += e.connectFailures;
        out.tlsFailures += e.tlsFailures;
        out.requestTimeouts += e.requestTimeouts;
        out.poolExhausted += e.poolExhausted;
        out.otherErrors += e.otherErrors;
        out.reconnects += e.reconnects;
        out.coalesceHits += e.coalesceHits;
        out.bytesOut += e.bytesOut;
        out.bytesIn += e.bytesIn;
        out.slotsInUse += e.slotsInUse;
    }

    return out;
}

// Latency histograms add elementwise: a percentile computed off the summed buckets is the same as
// over the merged raw samples, so summing across workers loses nothing.
static Shared::LatencyMetrics AggregateLatency(std::uint16_t idx, std::uint16_t cap,
                                               Shared::LatencyMetrics* (*slots)(int)) noexcept
{
    Shared::LatencyMetrics out{};
    if(!GlobalLatencyEnabled || idx >= cap)
        return out;

    for(int i = 0; i < GlobalWorkerCount; ++i) {
        const Shared::LatencyMetrics* arr = slots(i);
        if(!arr)
            continue;

        const Shared::LatencyMetrics& l = arr[idx];
        out.sumUs += l.sumUs;
        for(std::uint32_t b = 0; b < Shared::LATENCY_BUCKET_COUNT; ++b)
            out.buckets[b] += l.buckets[b];
    }

    return out;
}

Shared::LatencyMetrics AggregateRouteLatency(std::uint16_t routeIdx) noexcept
{
    return AggregateLatency(routeIdx, GlobalMaxRoutes, RouteLatencySlots);
}

Shared::LatencyMetrics AggregateEndpointLatency(std::uint16_t endpointIdx) noexcept
{
    return AggregateLatency(endpointIdx, GlobalMaxEndpoints, EndpointLatencySlots);
}

} // namespace MetricTracer
} // namespace WFX::Utils