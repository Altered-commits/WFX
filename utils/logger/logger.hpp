#ifndef WFX_UTILS_LOGGER_HPP
#define WFX_UTILS_LOGGER_HPP

#include <cstdio>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <type_traits>

#define WFX_LOG_ALL      Logger::ALL_MASK
#define WFX_LOG_WARNINGS Logger::WARN_MASK | Logger::ERROR_MASK | Logger::FATAL_MASK
#define WFX_LOG_INFO     Logger::INFO_MASK
#define WFX_LOG_NONE     Logger::NONE_MASK

namespace WFX::Utils {

class Logger {
public:
    using LevelMask = uint32_t;

    enum class Level : uint8_t {
        TRACE,
        DEBUG,
        INFO,
        WARN,
        ERR,
        FATAL,
        NONE
    };

    enum : LevelMask {
        TRACE_MASK = 1 << static_cast<int>(Level::TRACE),
        DEBUG_MASK = 1 << static_cast<int>(Level::DEBUG),
        INFO_MASK  = 1 << static_cast<int>(Level::INFO),
        WARN_MASK  = 1 << static_cast<int>(Level::WARN),
        ERROR_MASK = 1 << static_cast<int>(Level::ERR),
        FATAL_MASK = 1 << static_cast<int>(Level::FATAL),
        ALL_MASK   = TRACE_MASK | DEBUG_MASK | INFO_MASK | WARN_MASK | ERROR_MASK | FATAL_MASK,
        NONE_MASK  = 0
    };

public:
    static Logger& GetInstance();

    void SetLevelMask(LevelMask mask) { levelMask_ = mask; }
    void EnableTimestamps(bool enabled) { useTimestamps_ = enabled; }

    // Public variadic logging APIs
    template <typename... Args> void Print(Args&&... args) { Log<false>(Level::INFO, std::forward<Args>(args)...); }
    template <typename... Args> void Trace(Args&&... args) { Log(Level::TRACE,       std::forward<Args>(args)...); }
    template <typename... Args> void Debug(Args&&... args) { Log(Level::DEBUG,       std::forward<Args>(args)...); }
    template <typename... Args> void Info (Args&&... args) { Log(Level::INFO,        std::forward<Args>(args)...); }
    template <typename... Args> void Warn (Args&&... args) { Log(Level::WARN,        std::forward<Args>(args)...); }
    template <typename... Args> void Error(Args&&... args) { Log(Level::ERR,         std::forward<Args>(args)...); }
    template <typename... Args> void Fatal(Args&&... args) { Log(Level::FATAL,       std::forward<Args>(args)...); 
                                                                                     std::fflush(stderr);
                                                                                     std::exit(EXIT_FAILURE); }

private:
    Logger() = default;
    ~Logger() = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    const char* LevelToString(Level level) const;
    void CurrentTimestamp(char* buf, size_t len) const;

    template <bool LogLevel = true, typename... Args>
    void Log(Level level, Args&&... args);

    template <typename T>
    void PrintArg(FILE* out, T&& arg);

private:
    std::atomic<LevelMask> levelMask_     = ALL_MASK;
    std::atomic<bool>      useTimestamps_ = true;
    std::mutex             logMutex_;
};

} // namespace WFX::Utils

// Template method definitions must go in the header
namespace WFX::Utils {

// ---- Type-safe PrintArg overload ----
template <typename T>
void Logger::PrintArg(FILE* out, T&& arg) {
    using U = std::decay_t<T>;

    if constexpr(std::is_same_v<U, const char*> || std::is_same_v<U, char*>)
        std::fprintf(out, "%s", arg ? arg : "(null)");
    
    else if constexpr(std::is_same_v<U, std::string>)
        std::fprintf(out, "%s", arg.c_str());

    else if constexpr(std::is_same_v<U, std::string_view>)
        std::fprintf(out, "%.*s", static_cast<int>(arg.size()), arg.data());

    else if constexpr(std::is_same_v<U, char> || std::is_same_v<U, std::int8_t> || std::is_same_v<U, std::uint8_t>)
        std::fputc(arg, out);

    else if constexpr(std::is_integral_v<U>) {
        if constexpr(std::is_signed_v<U>)
            std::fprintf(out, "%lld", static_cast<long long>(arg));
        else
            std::fprintf(out, "%llu", static_cast<unsigned long long>(arg));
    }
    
    else if constexpr(std::is_floating_point_v<U>)
        std::fprintf(out, "%f", static_cast<double>(arg));
    
    // fallback: print pointer
    else
        std::fprintf(out, "%p", (const void*)&arg);
}

template <bool PureLog = true, typename... Args>
void Logger::Log(Level level, Args&&... args)
{
    LevelMask mask = 1 << static_cast<int>(level);
    if((levelMask_.load() & mask) == 0)
        return;

    std::lock_guard<std::mutex> lock(logMutex_);
    FILE* out = (level >= Level::WARN) ? stderr : stdout;

    if(useTimestamps_ && PureLog) {
        char ts[32];
        CurrentTimestamp(ts, sizeof(ts));
        std::fprintf(out, "[%s] ", ts);
    }

    if constexpr(PureLog)
        std::fprintf(out, "[%s] ", LevelToString(level));

    // Fold expression for variadic print
    (PrintArg(out, std::forward<Args>(args)), ...);

    std::fputc('\n', out);
}

} // namespace WFX::Utils

#endif // WFX_UTILS_LOGGER_HPP