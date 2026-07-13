// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "utils_api.hpp"
#include "utils/diagnostics/logger.hpp"
#include "utils/diagnostics/metric_tracer.hpp"

namespace WFX::Shared {

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
        []() -> LogMetrics {
            auto* m = Utils::MetricTracer::Current();
            return m ? m->log : LogMetrics{};
        },
        []() -> NetworkMetrics {
            auto* m = Utils::MetricTracer::Current();
            return m ? m->network : NetworkMetrics{};
        },
        []() -> SelfMetrics {
            auto* m = Utils::MetricTracer::Current();
            return m ? m->self : SelfMetrics{};
        },
        []() -> LogMetrics     { return Utils::MetricTracer::AggregateLog(); },
        []() -> NetworkMetrics { return Utils::MetricTracer::AggregateNetwork(); },
        []() -> SelfMetrics    { return Utils::MetricTracer::AggregateSelf(); }
    };
    // clang-format on

    return &GlobalUtilsAPIExt1;
}

} // namespace WFX::Shared