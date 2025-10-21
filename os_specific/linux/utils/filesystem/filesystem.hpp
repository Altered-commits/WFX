#ifndef WFX_LINUX_FILESYSTEM_HPP
#define WFX_LINUX_FILESYSTEM_HPP

#include "utils/filesystem/filesystem.hpp"

namespace WFX::OSSpecific {

using namespace WFX::Utils; // For 'BaseFileSystem', 'BaseFile'

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
    void OpenExisting(int fd, std::size_t size);

private:
    int         fd_       = -1;
    bool        existing_ = false;
    std::size_t size_     = 0;
};

class LinuxFileSystem : public BaseFileSystem {
public:
    // File Manipulation
    bool        FileExists(const char* path)                 const override;
    bool        DeleteFile(const char* path)                 const override;
    bool        RenameFile(const char* from, const char* to) const override;
    std::size_t GetFileSize(const char* path)                const override;

    // File Handling
    BaseFilePtr OpenFileRead(const char* path, bool inBinaryMode)        override;
    BaseFilePtr OpenFileWrite(const char* path, bool inBinaryMode)       override;
    BaseFilePtr OpenFileExisting(WFXFileDescriptor fd)                   override;
    BaseFilePtr OpenFileExisting(WFXFileDescriptor fd, std::size_t size) override;

    // Directory Manipulation
    bool          DirectoryExists(const char* path)                                                 const override;
    bool          CreateDirectory(std::string path, bool recurseParentDir)                          const override;
    bool          DeleteDirectory(const char* path)                                                 const override;
    DirectoryList ListDirectory(std::string path, bool shouldRecurse)                               const override;
    void          ListDirectory(std::string path, bool shouldRecurse, const FileCallback& callback) const override;

private: // Helper functions
    void ListDirectoryImpl(std::string& path, bool shouldRecurse, const FileCallback& callback) const;
};

} // namespace WFX::OSSpecific

#endif // WFX_LINUX_FILESYSTEM_HPP