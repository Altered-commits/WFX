#ifndef WFX_UTILS_FILESYSTEM_HPP
#define WFX_UTILS_FILESYSTEM_HPP

#include "utils/common/file.hpp"
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <cstdint>

// Dawg 'Windows' is hurting my brain, why name it same as mine smh
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

// vvv Main Functionality vvv
namespace FileSystem {

// File Manipulation
bool        CreateFile(const char* path);
bool        FileExists(const char* path);
bool        DeleteFile(const char* path);
bool        RenameFile(const char* from, const char* to);
std::size_t GetFileSize(const char* path);

// Open file for reading/writing: returns RAII-wrapped BaseFile
BaseFilePtr OpenFileRead(const char* path, bool inBinaryMode = false);
BaseFilePtr OpenFileWrite(const char* path, bool inBinaryMode = false);
BaseFilePtr OpenFileExisting(WFXFileDescriptor fd, bool fromCache = true);
BaseFilePtr OpenFileExisting(WFXFileDescriptor fd, std::size_t size, bool fromCache = true);

// Directory Manipulation
bool          DirectoryExists(const char* path);
bool          CreateDirectory(std::string path, bool recurseParentDir = true);
bool          DeleteDirectory(const char* path);
DirectoryList ListDirectory(std::string path, bool shouldRecurse);
void          ListDirectory(std::string path, bool shouldRecurse, const FileCallback& onEntry);

} // namespace FileSystem

} // namespace WFX::Utils

#endif // WFX_UTILS_FILESYSTEM_HPP