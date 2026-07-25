// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "utils_api.hpp"
#include "http/connection/http_connection.hpp"
#include "http/routing/router.hpp"
#include "utils/diagnostics/logger.hpp"
#include "utils/diagnostics/metric_tracer.hpp"

namespace WFX::Shared {

// Registration data the route/endpoint getters read identity from. Safe to hold as plain pointers-
// -for the same reason the http/endpoint tables are: the flow is single threaded and stays that way
static Http::Router* GlobalMetricsRouter = nullptr;
static Http::HttpConnectionHandler* GlobalMetricsConnHandler = nullptr;

// vvv Main Shit vvv
const UtilsAPIExt1* GetUtilsAPIExt1()
{
    // clang-format off
    // NOLINTNEXTLINE(readability-identifier-naming) - singleton table, treated as Global variable
    static const UtilsAPIExt1 GlobalUtilsAPIExt1 = {
        // vvv Logging vvv
        [](const char* m) { Utils::GetLogger().Trace(m); },
        [](const char* m) { Utils::GetLogger().Debug(m); },
        [](const char* m) { Utils::GetLogger().Info(m); },
        [](const char* m) { Utils::GetLogger().Warn(m); },
        [](const char* m) { Utils::GetLogger().Error(m); },
        [](const char* m) { Utils::GetLogger().Fatal(m); },

        // vvv Metrics vvv
        []() -> std::uint16_t { return static_cast<std::uint16_t>(Utils::MetricTracer::GlobalWorkerCount); },
        []() -> std::uint16_t {
            const int idx = Utils::MetricTracer::GlobalWorkerIndex;
            return idx < 0 ? 0 : static_cast<std::uint16_t>(idx);
        },
        [](std::uint16_t w) -> LogMetrics {
            auto* m = Utils::MetricTracer::Slot(static_cast<int>(w));
            return m ? m->log : LogMetrics{};
        },
        [](std::uint16_t w) -> NetworkMetrics {
            auto* m = Utils::MetricTracer::Slot(static_cast<int>(w));
            return m ? m->network : NetworkMetrics{};
        },
        [](std::uint16_t w) -> SelfMetrics {
            auto* m = Utils::MetricTracer::Slot(static_cast<int>(w));
            return m ? m->self : SelfMetrics{};
        },
        []() -> LogMetrics     { return Utils::MetricTracer::AggregateLog(); },
        []() -> NetworkMetrics { return Utils::MetricTracer::AggregateNetwork(); },
        []() -> SelfMetrics    { return Utils::MetricTracer::AggregateSelf(); },

        // vvv Route / Endpoint metrics vvv
        []() -> std::uint16_t {
            return GlobalMetricsRouter ? GlobalMetricsRouter->RouteCount() : 0;
        },
        []() -> std::uint16_t {
            return GlobalMetricsConnHandler ? GlobalMetricsConnHandler->EndpointCount() : 0;
        },
        [](std::uint16_t idx) -> RouteMetricsView {
            RouteMetricsView view{};
            if(!GlobalMetricsRouter || idx >= GlobalMetricsRouter->RouteCount())
                return view;

            const Http::RouteView route = GlobalMetricsRouter->RouteAt(idx);

            view.path = StringView{route.path.data(), static_cast<std::uint64_t>(route.path.size())};
            view.method = route.method;
            view.metrics = Utils::MetricTracer::AggregateRoute(idx);

            return view;
        },
        [](std::uint16_t idx) -> EndpointMetricsView {
            EndpointMetricsView view{};
            if(!GlobalMetricsConnHandler || idx >= GlobalMetricsConnHandler->EndpointCount())
                return view;

            view.host = GlobalMetricsConnHandler->EndpointHostAt(idx);
            view.metrics = Utils::MetricTracer::AggregateEndpoint(idx);

            return view;
        },
        [](std::uint16_t idx) -> LatencyMetrics { return Utils::MetricTracer::AggregateRouteLatency(idx); },
        [](std::uint16_t idx) -> LatencyMetrics { return Utils::MetricTracer::AggregateEndpointLatency(idx); },
        []() -> bool { return Utils::MetricTracer::LatencyEnabled(); }
    };
    // clang-format on

    return &GlobalUtilsAPIExt1;
}

void InitUtilsAPIExt1(Http::Router* router, Http::HttpConnectionHandler* connHandler)
{
    GlobalMetricsRouter = router;
    GlobalMetricsConnHandler = connHandler;
}

} // namespace WFX::Shared