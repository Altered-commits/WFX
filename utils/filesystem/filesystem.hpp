#ifndef WFX_UTILS_FILESYSTEM_HPP
#define WFX_UTILS_FILESYSTEM_HPP

#include "utils/common/file.hpp"
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <cstdint>

// Dawg Windows is hurting my brain
// Like i do not even include stuff related to Windows in this file yet it SOMEHOW picks up the winapi macros
#undef DeleteFile
#undef CreateDirectory

namespace WFX::Utils {

class BaseFile;

using FileCallback  = std::function<void(std::string)>;
using DirectoryList = std::vector<std::string>;
using BaseFilePtr   = std::unique_ptr<BaseFile>;

class BaseFile {
public:
    virtual ~BaseFile() = default;

    // Close file (RAII handles this automatically)
    virtual void Close() = 0;

    // Reading / Writing chunks
    virtual std::int64_t Read(void* buffer, std::size_t bytes)        = 0;
    virtual std::int64_t Write(const void* buffer, std::size_t bytes) = 0;
    virtual std::int64_t ReadAt(void* buffer, std::size_t bytes, std::size_t offset)        = 0;
    virtual std::int64_t WriteAt(const void* buffer, std::size_t bytes, std::size_t offset) = 0;

    // Seek / Tell
    virtual bool         Seek(std::size_t offset) = 0;
    virtual std::int64_t Tell() const             = 0;

    // Utility
    virtual std::size_t Size()   const = 0;
    virtual bool        IsOpen() const = 0;
};

class BaseFileSystem {
public:
    virtual ~BaseFileSystem() = default;

    // File Manipulation
    virtual bool        FileExists(const char* path)                 const = 0;
    virtual bool        DeleteFile(const char* path)                 const = 0;
    virtual bool        RenameFile(const char* from, const char* to) const = 0;
    virtual std::size_t GetFileSize(const char* path)                const = 0;

    // Open file for reading/writing: returns RAII-wrapped BaseFile
    virtual BaseFilePtr OpenFileRead(const char* path, bool inBinaryMode = false)  = 0;
    virtual BaseFilePtr OpenFileWrite(const char* path, bool inBinaryMode = false) = 0;
    virtual BaseFilePtr OpenFileExisting(WFXFileDescriptor fd)                        = 0;
    virtual BaseFilePtr OpenFileExisting(WFXFileDescriptor fd, std::size_t size)      = 0;

    // Directory Manipulation
    virtual bool          DirectoryExists(const char* path)                                                const = 0;
    virtual bool          CreateDirectory(std::string path, bool recurseParentDir = true)                  const = 0;
    virtual bool          DeleteDirectory(const char* path)                                                const = 0;
    virtual DirectoryList ListDirectory(std::string path, bool shouldRecurse)                              const = 0;
    virtual void          ListDirectory(std::string path, bool shouldRecurse, const FileCallback& onEntry) const = 0;
};

// MAIN SHIT: Returns singleton reference to stuff needed for File / Directory manipulation
// Also using this as namespace lmao
class FileSystem final {
public:
    static BaseFileSystem& GetFileSystem();

private:
    FileSystem()  = delete;
    ~FileSystem() = delete;
};

} // namespace WFX::Utils

#endif // WFX_UTILS_FILESYSTEM_HPP