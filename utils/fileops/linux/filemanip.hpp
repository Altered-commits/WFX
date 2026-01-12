#ifndef WFX_LINUX_FILEMANIP_HPP
#define WFX_LINUX_FILEMANIP_HPP

#include "utils/fileops/filesystem.hpp"

// Including them here because these will be common for both filesystem.cpp and filemanip.cpp
// And this file will STRICTLY be included in those two files only so no issue :)
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace WFX::Utils {

class LinuxFile : public BaseFile {
public:
    LinuxFile() = default;
    ~LinuxFile() override;

public:
    std::int64_t Read(void* buffer, std::size_t bytes)        override;
    std::int64_t Write(const void* buffer, std::size_t bytes) override;

    std::int64_t ReadAt(void* buffer, std::size_t bytes, std::size_t offset)        override;
    std::int64_t WriteAt(const void* buffer, std::size_t bytes, std::size_t offset) override;

    bool         Seek(std::size_t offset) override;

    std::int64_t Tell()             const override;
    std::size_t  Size()             const override;

    void         Close()                  override;
    bool         IsOpen()           const override;

public: // For internal public use
    bool OpenRead(const char* path);
    bool OpenWrite(const char* path);
    void OpenExisting(int fd, std::size_t size, bool cached);

private:
    int         fd_       = -1;
    bool        existing_ = false;
    bool        cached_   = false;
    std::size_t size_     = 0;
};

} // namespace WFX::Utils

#endif // WFX_LINUX_FILEMANIP_HPP