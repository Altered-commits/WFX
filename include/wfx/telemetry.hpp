// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_TELEMETRY_HPP
#define WFX_INC_WFX_TELEMETRY_HPP

// -----------------------------------------------------------------------
// wfx/telemetry.hpp
// Zero-alloc logging and metrics for user code.
//
// Logging crosses the ABI boundary as a single null-terminated string.
// Metrics are aggregated across all workers via shared mmap and returned-
// -as plain structs.
//
// Provides:
//   WFX::LogTrace  / WFX::LogDebug  / WFX::LogInfo
//   WFX::LogWarn   / WFX::LogError  / WFX::LogFatal
//
//   WFX::GetLogMetrics()          -- this worker's log counters
//   WFX::GetNetworkMetrics()      -- this worker's network counters
//   WFX::GetLogMetricsAll()       -- aggregated across all workers
//   WFX::GetNetworkMetricsAll()   -- aggregated across all workers
//
// Usage:
//   WFX::LogInfo("[MyHandler]: started on port ", port);
//   WFX::LogWarn("[Auth]: token expiring in ", seconds, "s");
//   WFX::LogError("[DB]: query failed, code=", code);
//   WFX::LogFatal("[Init]: cannot continue");  // aborts
//
//   auto net = WFX::GetNetworkMetricsAll();
//   WFX::LogInfo("total requests: ", net.requests);
// -----------------------------------------------------------------------

#include "core/core.hpp"
#include "http/response.hpp"
#include "shared/utils/compiler_macro.hpp"
#include <charconv>
#include <type_traits>

namespace WFX {

namespace Detail {

// -----------------------------------------------------------------------
// Stack buffer size for formatted log lines
// Matches engine-side 'kMsgBufSize' (lines longer than this are truncated)
// -----------------------------------------------------------------------
static constexpr std::size_t kLogBufSize = 1024;

// vvv Fmt overloads vvv
inline char* LogFmt(char* p, char* end, std::string_view sv) noexcept
{
    const std::size_t n = std::min(static_cast<std::size_t>(end - p), sv.size());
    std::memcpy(p, sv.data(), n);
    return p + n;
}

inline char* LogFmt(char* p, char* end, const char* s) noexcept
{
    return LogFmt(p, end, std::string_view(s ? s : "(null)"));
}

inline char* LogFmt(char* p, char* end, char c) noexcept
{
    if(p < end)
        *p++ = c;
    return p;
}

inline char* LogFmt(char* p, char* end, bool v) noexcept
{
    return LogFmt(p, end, v ? "true" : "false");
}

template <typename T>
inline std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>, char*> LogFmt(
    char* p, char* end, T value) noexcept
{
    if(end - p < 24)
        return p;

    auto [ep, ec] = std::to_chars(p, p + 24, value);
    return ec == std::errc() ? ep : p;
}

template <typename T>
inline std::enable_if_t<std::is_floating_point_v<T>, char*> LogFmt(char* p, char* end, T value) noexcept
{
    if(end - p < 32)
        return p;

    auto [ep, ec] = std::to_chars(p, p + 32, value);
    return ec == std::errc() ? ep : p;
}

template <typename T>
inline std::enable_if_t<std::is_pointer_v<T> && !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>,
                        char*>
LogFmt(char* p, char* end, T value) noexcept
{
    if(end - p < 20)
        return p;

    *p++ = '0';
    *p++ = 'x';

    auto [ep, ec] = std::to_chars(p, p + 16, reinterpret_cast<std::uintptr_t>(value), 16);
    return ec == std::errc() ? ep : p;
}

// -----------------------------------------------------------------------
// Formats args into a stack buffer, crosses ABI boundary once via a-
// -single (const char*) call. 'lvl' maps directly to LogFn index
// -----------------------------------------------------------------------
template <typename... Args> inline void LogEmit(int lvl, Args&&... args) noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return;

    char buf[kLogBufSize];
    char* p = buf;
    char* end = p + kLogBufSize - 1;

    ((p = LogFmt(p, end, std::forward<Args>(args))), ...);

    // Signal truncation if we hit the limit
    if(p >= end - 1) {
        buf[kLogBufSize - 5] = '.';
        buf[kLogBufSize - 4] = '.';
        buf[kLogBufSize - 3] = '.';
        buf[kLogBufSize - 2] = '\0';
    }
    else
        *p = '\0';

    switch(lvl) {
        case 0:
            api->LogTrace(buf);
            break;
        case 1:
            api->LogDebug(buf);
            break;
        case 2:
            api->LogInfo(buf);
            break;
        case 3:
            api->LogWarn(buf);
            break;
        case 4:
            api->LogError(buf);
            break;
        case 5:
            api->LogFatal(buf);
            break;
    }
}

} // namespace Detail

// vvv Log API vvv
template <typename... Args> inline void LogTrace(Args&&... args) noexcept
{
    Detail::LogEmit(0, std::forward<Args>(args)...);
}

template <typename... Args> inline void LogDebug(Args&&... args) noexcept
{
    Detail::LogEmit(1, std::forward<Args>(args)...);
}

template <typename... Args> inline void LogInfo(Args&&... args) noexcept
{
    Detail::LogEmit(2, std::forward<Args>(args)...);
}

template <typename... Args> inline void LogWarn(Args&&... args) noexcept
{
    Detail::LogEmit(3, std::forward<Args>(args)...);
}

template <typename... Args> inline void LogError(Args&&... args) noexcept
{
    Detail::LogEmit(4, std::forward<Args>(args)...);
}

template <typename... Args> [[noreturn]] inline void LogFatal(Args&&... args) noexcept
{
    Detail::LogEmit(5, std::forward<Args>(args)...);
    WFX_UNREACHABLE;
}

// vvv Metrics API vvv
inline Shared::LogMetrics GetLogMetrics() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->GetLogMetricsWorker();
}

inline Shared::NetworkMetrics GetNetworkMetrics() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->GetNetMetricsWorker();
}

inline Shared::SelfMetrics GetProcessMetrics() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->GetSelfMetricsWorker();
}

inline Shared::LogMetrics GetLogMetricsAll() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->GetLogMetricsAggregate();
}

inline Shared::NetworkMetrics GetNetworkMetricsAll() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->GetNetMetricsAggregate();
}

inline Shared::SelfMetrics GetProcessMetricsAll() noexcept
{
    const auto* api = Core::UtilsApiExt1();
    if(!api)
        return {};

    return api->GetSelfMetricsAggregate();
}

} // namespace WFX

#endif // WFX_INC_WFX_TELEMETRY_HPP