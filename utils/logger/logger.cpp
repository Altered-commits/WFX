#include "logger.hpp"

#include <ctime>
#include <chrono>

namespace WFX::Utils {

Logger& Logger::GetInstance()
{
    static Logger loggerInstance;
    return loggerInstance;
}

const char* Logger::LevelToString(Level level) const
{
    switch (level) {
        case Level::TRACE: return "TRACE";
        case Level::DEBUG: return "DEBUG";
        case Level::INFO:  return "INFO";
        case Level::WARN:  return "WARN";
        case Level::ERR:   return "ERROR";
        case Level::FATAL: return "FATAL";
        default:           return "UNKNOWN";
    }
}

void Logger::CurrentTimestamp(char* buf, size_t len) const
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t   = system_clock::to_time_t(now);
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::snprintf(buf, len, "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, (int)ms.count());
}

} // namespace WFX::Utils