#include "utils/diagnostics/logger.hpp"

namespace WFX::Utils {

// Heap allocated intentionally as logger must outlive all other globals
// Static destruction order across translation units is undefined, so any-
// -global that logs in its destructor would crash if logger destructed first
Logger& GetLogger() noexcept
{
    static Logger* __GlobalLogger = new Logger();
    return *__GlobalLogger;
}

// vvv Constructor vvv
Logger::Logger()
{
    colors_ = WFX_IS_TTY();
}

// vvv Main Functions vvv
void Logger::SetLevelMask(LevelMask mask) noexcept { levelMask_  = mask; }
void Logger::SetMinLevel(Level lvl)       noexcept { levelMask_  = ALL_MASK << static_cast<std::uint8_t>(lvl); }
void Logger::EnableTimestamps(bool v)     noexcept { timestamps_ = v; }
void Logger::EnableStdout(bool v)         noexcept { stdout_     = v; }

void Logger::EnableColors(bool v) noexcept
{
    colors_ = v && WFX_IS_TTY();
}

bool Logger::OpenFile(const char* path, std::size_t maxBytes, int keepFiles) noexcept
{
    return fileSink_.Open(path, maxBytes, keepFiles);
}

// vvv Helper Functions vvv
//  Logger
#ifndef _WIN32
void Logger::WriteRetry(int fd, const char* data, std::size_t len) noexcept
{
    while(len > 0) {
        const ssize_t n = ::write(fd, data, len);
        if(n >= 0) {
            data += n;
            len  -= static_cast<std::size_t>(n);
        }
        else if(errno != EINTR)
            break;
    }
}
#endif

//  TimestampCache
void TimestampCache::Sync(std::chrono::steady_clock::time_point now) noexcept
{
    using namespace std::chrono;

    syncPoint_ = now;
    synced_    = true;

    const auto wall = system_clock::now();
    const auto tt   = system_clock::to_time_t(wall);

    epochMs_ = static_cast<int>(
        duration_cast<milliseconds>(wall.time_since_epoch()).count() % 1000
    );

    std::tm tm{};
    WFX_LOCALTIME(&tm, &tt);

    cachedHour_ = tm.tm_hour;
    cachedMin_  = tm.tm_min;
    cachedSec_  = tm.tm_sec;
}

//  CircularFileSink
void CircularFileSink::CloseInternal() noexcept
{
    if(!file_)
        return;

    // Call Close() explicitly before reset so LinuxFile gets fd_ = -1-
    // -before the unique_ptr destructor runs. This prevents double close-
    // -if the LinuxFile destructor also calls Close()
    file_->Close();
    file_.reset();
}

bool CircularFileSink::Open(const char* path, std::size_t maxBytes, int keepFiles) noexcept
{
    maxBytes_  = maxBytes;
    keepFiles_ = (keepFiles > 0 && keepFiles <= kMaxKeep) ? keepFiles : kDefaultKeepFiles;

    std::strncpy(path_, path, sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = '\0';

    return OpenFresh();
}

void CircularFileSink::Write(const char* data, std::size_t len) noexcept
{
    if(!IsOpen()) return;
    if(file_->Size() + len >= maxBytes_) Rotate();
    file_->Write(data, len);
}

bool CircularFileSink::OpenFresh() noexcept
{
    file_ = FileSystem::OpenFileWrite(path_, true);
    return IsOpen();
}

void CircularFileSink::Rotate() noexcept
{
    CloseInternal();

    char src[512];
    char dst[512];

    for(int i = keepFiles_ - 1; i >= 1; --i) {
        std::snprintf(src, sizeof(src), "%s.%d", path_, i);
        std::snprintf(dst, sizeof(dst), "%s.%d", path_, i + 1);
        FileSystem::RenameFile(src, dst);
    }

    std::snprintf(dst, sizeof(dst), "%s.1", path_);
    FileSystem::RenameFile(path_, dst);

    OpenFresh();
}

} // namespace WFX::Utils