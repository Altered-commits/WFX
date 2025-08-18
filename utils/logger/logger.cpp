#include "logger.hpp"
#include <iomanip>

namespace WFX::Utils {

Logger& Logger::GetInstance()
{
    static Logger loggerInstance;
    return loggerInstance;
}

void Logger::SetLevelMask(LevelMask mask)
{
    levelMask_ = mask;
}

void Logger::EnableTimestamps(bool enabled)
{
    useTimestamps_ = enabled;
}

const char* Logger::LevelToString(Level level) const
{
    switch(level) {
        case Level::TRACE: return "TRACE";
        case Level::DEBUG: return "DEBUG";
        case Level::INFO:  return "INFO";
        case Level::WARN:  return "WARN";
        case Level::ERR:   return "ERR";
        case Level::FATAL: return "FATAL";
        default:           return "UNKNOWN";
    }
}

std::string Logger::CurrentTimestamp() const
{
    using namespace std::chrono;

    auto now  = system_clock::now();
    auto time = system_clock::to_time_t(now);
    auto ms   = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

} // namespace WFX::Utils