// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_CRASH_TRACER_HPP
#define WFX_UTILS_CRASH_TRACER_HPP

#include <cstdint>
#include <cstddef>
#include "shared/utils/detection_macro.hpp"

#if defined(WFX_PLATFORM_POSIX)
#include <signal.h>
#elif defined(WFX_PLATFORM_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#if defined(WFX_DEBUG)
#define WFX_TRACE()                                                                                                    \
    WFX::Utils::CrashTracer::ScopedFrame __WFXSF_                                                                      \
    {                                                                                                                  \
        __func__, __FILE__, __LINE__                                                                                   \
    }
#else
#define WFX_TRACE() ((void)0)
#endif
#define WFX_CHECKPOINT(label) WFX::Utils::CrashTracer::UpdateTop(label, __FILE__, __LINE__)

namespace WFX::Utils {

class CrashTracer {
public:
    static constexpr int MAX_DEPTH = 48;

    struct Frame {
        const char* func;
        const char* file;
        int line;
    };

    struct ScopedFrame {
        ScopedFrame(const char* func, const char* file, int line) noexcept
        {
            savedDepth_ = depth_;
            int d = depth_;

            if(d < MAX_DEPTH) {
                frames_[d] = {func, file, line};
                depth_ = d + 1;
            }
        }
        ~ScopedFrame() noexcept
        {
            depth_ = savedDepth_;
        }

    private:
        int savedDepth_;
    };

    static void UpdateTop(const char* label, const char* file, int line) noexcept
    {
        int d = depth_ - 1;
        if(d >= 0) {
            frames_[d].func = label;
            frames_[d].file = file;
            frames_[d].line = line;
        }
    }

    static void Install(const char* logDir = nullptr) noexcept;
    static void SetWorkerName(const char* name) noexcept;

private:
    inline static Frame frames_[MAX_DEPTH];
    inline static int depth_;
    inline static char workerName_[32];
    inline static char logDir_[256];
    inline static char altStack_[65536];

private:
    struct UTCTime {
        int year, mon, day, hour, min, sec;
    };
    static UTCTime EpochToUTC(long long sec) noexcept;
    static long long GetEpochNow() noexcept;

private:
    static void BuildLogPath(char* out, std::size_t max, long long epoch) noexcept;
    static void WriteCrashBody(int fd, int sig, void* siginfo, void* uctx, long long epoch) noexcept;

    static void SafeWrite(int fd, const char* s) noexcept;
    static void SafeWriteInt(int fd, long long v) noexcept;
    static void SafeWriteHex(int fd, unsigned long long v) noexcept;
    static void SafeWriteFrame(int fd, const Frame& f, int i) noexcept;
    static void SafeWriteTimestamp(int fd, long long epoch) noexcept;

#if defined(WFX_PLATFORM_POSIX)
    static void InstallPosix() noexcept;
    static void PosixHandler(int sig, siginfo_t* info, void* uctx) noexcept;
    static void WriteRegisters(int fd, void* uctx) noexcept;
#elif defined(WFX_PLATFORM_WINDOWS)
    static void InstallWindows() noexcept;
    static LONG WINAPI WindowsVEHandler(EXCEPTION_POINTERS* ep) noexcept;
    static void WriteRegisters(int fd, CONTEXT* ctx) noexcept;
#endif
};

} // namespace WFX::Utils

#endif // WFX_UTILS_CRASH_TRACER_HPP