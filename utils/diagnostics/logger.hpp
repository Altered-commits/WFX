// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_LOGGER_HPP
#define WFX_UTILS_LOGGER_HPP

// Sinks (all optional, composable):
//   Stdout     : ANSI colors, gray timestamps, colored level tags
//   File       : plain text, no ANSI codes, circular / size-rotating
//
// NOT thread-safe by design. One instance per worker

#include "metric_tracer.hpp"
#include "utils/fileops/filesystem.hpp"
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <string_view>
#include <type_traits>

#include <errno.h>
#include <unistd.h>

#define WFX_IS_TTY() (::isatty(STDOUT_FILENO) != 0)
#define WFX_STDOUT_WRITE(data, len) WriteRetry(STDOUT_FILENO, (data), (len))
#define WFX_LOCALTIME(tm_ptr, tt_ptr) localtime_r((tt_ptr), (tm_ptr))
#define WFX_GMTIME(tm_ptr, tt_ptr) gmtime_r((tt_ptr), (tm_ptr))

namespace WFX::Utils {

// On Open()   : OpenFileWrite truncates and starts fresh. Size tracked via
//               file_->Size() which PosixFile maintains internally
//
// On rotation : close -> shift .N->.(N+1) down to .1->.2
//               -> rename current to .1 -> OpenFileWrite on fresh current
//
// Restart behaviour: truncates existing log, starts fresh
// Rotated copies (.1 .. .N) survive across restarts
class CircularFileSink {
public:
    static constexpr std::size_t K_DEFAULT_MAX_BYTES = 16 * 1024 * 1024;
    static constexpr int K_DEFAULT_KEEP_FILES = 4;
    static constexpr int K_MAX_KEEP = 32;

public:
    CircularFileSink() = default;
    ~CircularFileSink() = default;

    CircularFileSink(const CircularFileSink&) = delete;
    CircularFileSink& operator=(const CircularFileSink&) = delete;

public:
    bool IsOpen() const noexcept
    {
        return file_ && file_->IsOpen();
    }

    bool Open(const char* path, std::size_t maxBytes = K_DEFAULT_MAX_BYTES,
              int keepFiles = K_DEFAULT_KEEP_FILES) noexcept;
    void Write(const char* data, std::size_t len) noexcept;

private:
    bool OpenFresh() noexcept;
    void Rotate() noexcept;
    void CloseInternal() noexcept;

private:
    char path_[512] = {};
    std::size_t maxBytes_ = K_DEFAULT_MAX_BYTES;
    int keepFiles_ = K_DEFAULT_KEEP_FILES;
    BaseFilePtr file_ = nullptr;
};

// localtime_r/localtime_s costs 300-700ns (glibc mutex, tz lookup)
// Called once per second; sub-second tracked via steady_clock delta,
// which is a vDSO read + integer math (~10ns, no syscall).
class TimestampCache {
public:
    // Whichever zone the clock is read in. Set once at startup, before any line is written
    void UseUtc(bool v) noexcept
    {
        utc_ = v;
        synced_ = false;
    }

    // Writes [HH:MM:SS.mmm] into out. Returns updated pointer.
    // Caller must ensure >= 14 bytes available
    char* Format(char* out) noexcept
    {
        const int ms = Tick();

        *out++ = '[';
        out = W2(out, cachedHour_);
        *out++ = ':';
        out = W2(out, cachedMin_);
        *out++ = ':';
        out = W2(out, cachedSec_);
        *out++ = '.';
        out = W3(out, ms);
        *out++ = ']';

        return out;
    }

    // Writes RFC 3339, 2026-10-16T13:27:16.060+05:30 or the same with Z, so a line can be placed on
    // a clock rather than only within a day. Caller must ensure >= 29 bytes available
    char* FormatRFC3339(char* out) noexcept
    {
        const int ms = Tick();

        out = W4(out, cachedYear_);
        *out++ = '-';
        out = W2(out, cachedMonth_);
        *out++ = '-';
        out = W2(out, cachedDay_);
        *out++ = 'T';
        out = W2(out, cachedHour_);
        *out++ = ':';
        out = W2(out, cachedMin_);
        *out++ = ':';
        out = W2(out, cachedSec_);
        *out++ = '.';
        out = W3(out, ms);

        if(utc_) {
            *out++ = 'Z';
            return out;
        }

        *out++ = (offsetMinutes_ < 0) ? '-' : '+';

        const int offset = (offsetMinutes_ < 0) ? -offsetMinutes_ : offsetMinutes_;

        out = W2(out, offset / 60);
        *out++ = ':';
        out = W2(out, offset % 60);

        return out;
    }

private:
    // The second this line falls in, cached, and the milliseconds into it
    int Tick() noexcept
    {
        using namespace std::chrono;

        const auto now = steady_clock::now();
        const auto elapsed = duration_cast<milliseconds>(now - syncPoint_).count();

        if(elapsed >= 1000 || !synced_)
            Sync(now);

        return static_cast<int>((epochMs_ + elapsed) % 1000);
    }

    void Sync(std::chrono::steady_clock::time_point now) noexcept;

    static char* W2(char* p, int v) noexcept
    {
        *p++ = static_cast<char>('0' + v / 10);
        *p++ = static_cast<char>('0' + v % 10);
        return p;
    }

    static char* W3(char* p, int v) noexcept
    {
        *p++ = static_cast<char>('0' + (v / 100) % 10);
        *p++ = static_cast<char>('0' + (v / 10) % 10);
        *p++ = static_cast<char>('0' + v % 10);
        return p;
    }

    static char* W4(char* p, int v) noexcept
    {
        *p++ = static_cast<char>('0' + (v / 1000) % 10);
        *p++ = static_cast<char>('0' + (v / 100) % 10);
        *p++ = static_cast<char>('0' + (v / 10) % 10);
        *p++ = static_cast<char>('0' + v % 10);
        return p;
    }

private:
    std::chrono::steady_clock::time_point syncPoint_{};
    int cachedYear_ = 0;
    int cachedMonth_ = 0;
    int cachedDay_ = 0;
    int cachedHour_ = 0;
    int cachedMin_ = 0;
    int cachedSec_ = 0;
    int offsetMinutes_ = 0; // East of UTC, from tm_gmtoff, so a line says which clock wrote it
    int epochMs_ = 0;
    bool synced_ = false;
    bool utc_ = false;
};

// Memory layout per instance:
//   msgBuf_    : 1056 bytes, formatted message body (plain, no ANSI)
//   prefixBuf_ :   64 bytes, colored prefix for stdout only
// Total: ~1.1 KiB per worker instance
class Logger final {
public:
    using LevelMask = std::uint8_t;

    enum class Level : std::uint8_t { TRACE = 0, DEBUG, INFO, WARN, ERROR, FATAL, NONE };

    enum : LevelMask {
        TRACE_MASK = 1u << static_cast<std::uint8_t>(Level::TRACE),
        DEBUG_MASK = 1u << static_cast<std::uint8_t>(Level::DEBUG),
        INFO_MASK = 1u << static_cast<std::uint8_t>(Level::INFO),
        WARN_MASK = 1u << static_cast<std::uint8_t>(Level::WARN),
        ERROR_MASK = 1u << static_cast<std::uint8_t>(Level::ERROR),
        FATAL_MASK = 1u << static_cast<std::uint8_t>(Level::FATAL),

        ALL_MASK = TRACE_MASK | DEBUG_MASK | INFO_MASK | WARN_MASK | ERROR_MASK | FATAL_MASK,
        NONE_MASK = 0u
    };

public:
    Logger();
    ~Logger() = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

public:
    void SetLevelMask(LevelMask mask) noexcept;
    void SetMinLevel(Level lvl) noexcept;
    void EnableTimestamps(bool v) noexcept;
    void EnableColors(bool v) noexcept;
    void EnableStdout(bool v) noexcept;
    void EnableJson(bool v) noexcept;
    void EnableUtc(bool v) noexcept;

    bool OpenFile(const char* path, std::size_t maxBytes = CircularFileSink::K_DEFAULT_MAX_BYTES,
                  int keepFiles = CircularFileSink::K_DEFAULT_KEEP_FILES) noexcept;

public:
    template <typename... Args> void Trace(Args&&... args) noexcept
    {
        Emit(Level::TRACE, std::forward<Args>(args)...);
    }

    template <typename... Args> void Debug(Args&&... args) noexcept
    {
        Emit(Level::DEBUG, std::forward<Args>(args)...);
    }

    template <typename... Args> void Info(Args&&... args) noexcept
    {
        Emit(Level::INFO, std::forward<Args>(args)...);
    }

    template <typename... Args> void Warn(Args&&... args) noexcept
    {
        Emit(Level::WARN, std::forward<Args>(args)...);
    }

    template <typename... Args> void Error(Args&&... args) noexcept
    {
        Emit(Level::ERROR, std::forward<Args>(args)...);
    }

    template <typename... Args> [[noreturn]] void Fatal(Args&&... args) noexcept
    {
        Emit(Level::FATAL, std::forward<Args>(args)...);
        std::exit(1);
    }

    // Raw stdout print: no timestamp, no level tag, no sinks
    template <typename... Args> void Print(Args&&... args) noexcept
    {
        char* p = msgBuf_.data();
        char* end = p + K_MSG_BUF_SIZE - 1;

        ((p = Fmt(p, end, std::forward<Args>(args))), ...);
        *p++ = '\n';

        WFX_STDOUT_WRITE(msgBuf_.data(), static_cast<std::size_t>(p - msgBuf_.data()));
    }

private:
    struct LevelMeta {
        const char* tag;
        const char* ansi;
        const char* name; // What JSON calls it: the words Grafana and Loki already detect
    };

    static constexpr std::array<LevelMeta, 6> K_META{{{"[TRC]", "\033[90m", "trace"},
                                                      {"[DBG]", "\033[36m", "debug"},
                                                      {"[INF]", "\033[32m", "info"},
                                                      {"[WRN]", "\033[33m", "warn"},
                                                      {"[ERR]", "\033[31m", "error"},
                                                      {"[FTL]", "\033[35;1m", "fatal"}}};

    static constexpr const char* K_COLOR_GRAY = "\033[90m";
    static constexpr const char* K_COLOR_RESET = "\033[0m";

    // prefixBuf_ : colored prefix for stdout (timestamp + tag with ANSI)
    //              max: gray(5) + [HH:MM:SS.mmm](14) + reset(4) + sp(1)
    //                 + color(7) + [XYZ](5) + reset(4) + sp(1) = 41 bytes
    //              64 gives comfortable headroom
    static constexpr std::size_t K_PREFIX_BUF_SIZE = 64;

    // msgBuf_ : full plain line (timestamp + tag + message + newline)
    //           user message budget = 1024, prefix overhead = 32
    static constexpr std::size_t K_MSG_BUF_SIZE = 1024 + 32;

    // lineBuf_ : the JSON line, only ever written when that format is on
    //            message budget doubled, since " and \ cost two bytes each, plus the envelope
    static constexpr std::size_t K_LINE_BUF_SIZE = 2048 + 128;

private:
    template <typename... Args> void Emit(Level lvl, Args&&... args) noexcept
    {
        const LevelMask bit = 1u << static_cast<std::uint8_t>(lvl);

        if((levelMask_ & bit) == 0)
            return;

        const auto& meta = K_META[static_cast<std::uint8_t>(lvl)];

        char* mp = msgBuf_.data();
        char* end = mp + K_MSG_BUF_SIZE - 1;

        // JSON carries the time and the level as its own fields, so msgBuf_ holds only what the
        // caller wrote and EmitJson wraps it
        if(json_) {
            ((mp = Fmt(mp, end, std::forward<Args>(args))), ...);
            EmitJson(meta, std::string_view(msgBuf_.data(), static_cast<std::size_t>(mp - msgBuf_.data())));
            Count(lvl);

            return;
        }

        if(timestamps_) {
            mp = tsCache_.Format(mp);
            if(mp < end)
                *mp++ = ' ';
        }

        mp = RawCopy(mp, end, meta.tag);
        if(mp < end)
            *mp++ = ' ';

        const std::size_t prefixLen = static_cast<std::size_t>(mp - msgBuf_.data());

        ((mp = Fmt(mp, end, std::forward<Args>(args))), ...);
        *mp++ = '\n';

        const std::size_t totalLen = static_cast<std::size_t>(mp - msgBuf_.data());

        if(fileSink_.IsOpen())
            fileSink_.Write(msgBuf_.data(), totalLen);

        if(stdout_) {
            if(colors_) {
                char* cp = prefixBuf_.data();
                char* cend = cp + K_PREFIX_BUF_SIZE;

                if(timestamps_) {
                    cp = RawCopy(cp, cend, K_COLOR_GRAY);
                    cp = tsCache_.Format(cp);
                    cp = RawCopy(cp, cend, K_COLOR_RESET);
                    if(cp < cend)
                        *cp++ = ' ';
                }

                cp = RawCopy(cp, cend, meta.ansi);
                cp = RawCopy(cp, cend, meta.tag);
                cp = RawCopy(cp, cend, K_COLOR_RESET);
                if(cp < cend)
                    *cp++ = ' ';

                const std::size_t colorPrefixLen = static_cast<std::size_t>(cp - prefixBuf_.data());

                WFX_STDOUT_WRITE(prefixBuf_.data(), colorPrefixLen);
                WFX_STDOUT_WRITE(msgBuf_.data() + prefixLen, totalLen - prefixLen);
            }
            else
                WFX_STDOUT_WRITE(msgBuf_.data(), totalLen);
        }

        Count(lvl);
    }

    // IMPORTANT: This works assuming every metric is in the order:
    //   -> trace, debug, info, warn, error, fatal
    // Order is decided in 'shared/abis/types.hpp' -> 'LogMetrics'
    void Count(Level lvl) noexcept
    {
        if(auto* metrics = MetricTracer::Current()) {
            std::uint64_t* lines = &metrics->log.trace;
            lines[static_cast<std::uint8_t>(lvl)]++;
        }
    }

    void EmitJson(const LevelMeta& meta, std::string_view msg) noexcept
    {
        char* p = lineBuf_.data();

        // Two short of the end, so the closing quote, brace and newline always fit however much of
        // the message had to be dropped to get here
        char* end = p + K_LINE_BUF_SIZE - 4;

        p = RawCopy(p, end, "{\"time\":\"");
        p = tsCache_.FormatRFC3339(p);
        p = RawCopy(p, end, "\",\"level\":\"");
        p = RawCopy(p, end, meta.name);
        p = RawCopy(p, end, "\",\"msg\":\"");
        p = EscapeJson(p, end, msg);
        p = RawCopy(p, end, "\"}");
        *p++ = '\n';

        const auto len = static_cast<std::size_t>(p - lineBuf_.data());

        if(fileSink_.IsOpen())
            fileSink_.Write(lineBuf_.data(), len);

        // One write, whole line: two would let another worker's line land inside this one
        if(stdout_)
            WFX_STDOUT_WRITE(lineBuf_.data(), len);
    }

    // Escapes what JSON cannot carry raw. A message too long to fit escaped is cut rather than run
    // past the buffer, and never mid-escape, so the line is always one readable object
    static char* EscapeJson(char* p, char* end, std::string_view msg) noexcept
    {
        static constexpr char K_HEX[] = "0123456789abcdef";

        for(const char c : msg) {
            const auto byte = static_cast<unsigned char>(c);

            if(byte == '"' || byte == '\\') {
                if(end - p < 2)
                    break;

                *p++ = '\\';
                *p++ = static_cast<char>(byte);
            }
            else if(byte >= 0x20) {
                if(p >= end)
                    break;

                *p++ = static_cast<char>(byte);
            }
            else {
                if(end - p < 6)
                    break;

                *p++ = '\\';
                *p++ = 'u';
                *p++ = '0';
                *p++ = '0';
                *p++ = K_HEX[byte >> 4];
                *p++ = K_HEX[byte & 0x0F];
            }
        }

        return p;
    }

    static void WriteRetry(int fd, const char* data, std::size_t len) noexcept;

private:
    static char* RawCopy(char* p, char* end, const char* s) noexcept
    {
        while(*s && p < end)
            *p++ = *s++;
        return p;
    }

    static char* Fmt(char* p, char* end, std::string_view sv) noexcept
    {
        const std::size_t n = std::min(static_cast<std::size_t>(end - p), sv.size());
        std::memcpy(p, sv.data(), n);
        return p + n;
    }

    static char* Fmt(char* p, char* end, const std::string& s) noexcept
    {
        return Fmt(p, end, std::string_view(s));
    }

    static char* Fmt(char* p, char* end, const char* s) noexcept
    {
        return Fmt(p, end, std::string_view(s ? s : "(null)"));
    }

    static char* Fmt(char* p, char* end, char c) noexcept
    {
        if(p < end)
            *p++ = c;
        return p;
    }

    static char* Fmt(char* p, char* end, bool v) noexcept
    {
        return Fmt(p, end, v ? "true" : "false");
    }

    template <typename T>
    static std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>, char*> Fmt(
        char* p, char* end, T value) noexcept
    {
        if(end - p < 24)
            return p;
        auto [ep, ec] = std::to_chars(p, p + 24, value);
        return ec == std::errc() ? ep : p;
    }

    template <typename T>
    static std::enable_if_t<std::is_floating_point_v<T>, char*> Fmt(char* p, char* end, T value) noexcept
    {
        if(end - p < 32)
            return p;
        auto [ep, ec] = std::to_chars(p, p + 32, value);
        return ec == std::errc() ? ep : p;
    }

    template <typename T>
    static std::enable_if_t<std::is_pointer_v<T> && !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>,
                            char*>
    Fmt(char* p, char* end, T value) noexcept
    {
        if(end - p < 20)
            return p;
        p = RawCopy(p, end, "0x");
        auto [ep, ec] = std::to_chars(p, p + 16, reinterpret_cast<std::uintptr_t>(value), 16);
        return ec == std::errc() ? ep : p;
    }

private:
    LevelMask levelMask_ = ALL_MASK;

    bool timestamps_ = true;
    bool colors_ = true;
    bool stdout_ = true;
    bool json_ = false;

    std::array<char, K_MSG_BUF_SIZE> msgBuf_{};
    std::array<char, K_PREFIX_BUF_SIZE> prefixBuf_{};
    std::array<char, K_LINE_BUF_SIZE> lineBuf_{};

    TimestampCache tsCache_;
    CircularFileSink fileSink_;
};

// Free function declaration (defined in 'logger.cpp')
Logger& GetLogger() noexcept;

} // namespace WFX::Utils

#endif // WFX_UTILS_LOGGER_HPP