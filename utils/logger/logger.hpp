#ifndef WFX_UTILS_LOGGER_HPP
#define WFX_UTILS_LOGGER_HPP

#include "include/export_signature.hpp"

#include <string>
#include <mutex>
#include <iostream>
#include <chrono>
#include <ctime>
#include <sstream>
#include <atomic>
#include <cstdint>

#define WFX_LOG_ALL      Logger::ALL_MASK
#define WFX_LOG_WARNINGS Logger::WARN_MASK | Logger::ERROR_MASK | Logger::FATAL_MASK
#define WFX_LOG_INFO     Logger::INFO_MASK
#define WFX_LOG_NONE     Logger::NONE_MASK

namespace WFX::Utils {

class WFX_API Logger {
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

    void SetLevelMask(LevelMask mask);
    void EnableTimestamps(bool enabled);

    // Public variadic logging APIs
    template <typename... Args> void Trace(Args&&... args);
    template <typename... Args> void Debug(Args&&... args);
    template <typename... Args> void Info (Args&&... args);
    template <typename... Args> void Warn (Args&&... args);
    template <typename... Args> void Error(Args&&... args);
    template <typename... Args> void Fatal(Args&&... args);

private:
    Logger() = default;
    ~Logger() = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    const char* LevelToString(Level level) const;
    std::string CurrentTimestamp() const;

    template <typename... Args>
    void Log(Level level, Args&&... args);

private:
    std::atomic<LevelMask> levelMask_     = ALL_MASK;
    std::atomic<bool>      useTimestamps_ = true;
    std::mutex             logMutex_;
};

} // namespace WFX::Utils

// Template method definitions must go in the header
namespace WFX::Utils {

template <typename... Args>
void Logger::Trace(Args&&... args)
{
    Log(Level::TRACE, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::Debug(Args&&... args)
{
    Log(Level::DEBUG, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::Info(Args&&... args)
{
    Log(Level::INFO, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::Warn(Args&&... args)
{
    Log(Level::WARN, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::Error(Args&&... args)
{
    Log(Level::ERR, std::forward<Args>(args)...);
}

template <typename... Args>
void Logger::Fatal(Args&&... args)
{
    Log(Level::FATAL, std::forward<Args>(args)...);
    std::exit(EXIT_FAILURE);
}

template <typename... Args>
void Logger::Log(Level level, Args&&... args)
{
    LevelMask mask = 1 << static_cast<int>(level);
    if((levelMask_ & mask) == 0)
        return;

    std::lock_guard<std::mutex> lock(logMutex_);
    std::ostream& out = (level >= Level::WARN) ? std::cerr : std::cout;

    if(useTimestamps_)
        out << "[" << CurrentTimestamp() << "] ";

    // Variadic output
    out << "[" << LevelToString(level) << "] ";
    (out << ... << std::forward<Args>(args));
    out << '\n';
}

} // namespace WFX::Utils

#endif // WFX_UTILS_LOGGER_HPP