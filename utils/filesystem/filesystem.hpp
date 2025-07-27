#ifndef WFX_UTILS_FILESYSTEM_HPP
#define WFX_UTILS_FILESYSTEM_HPP

#include <string>
#include <functional>
#include <vector>

// Dawg Windows is hurting my brain
// Like i do not even include stuff related to Windows in this file yet it SOMEHOW picks up the winapi macros
#undef DeleteFile
#undef CreateDirectory

namespace WFX::Utils {

using FileCallback  = std::function<void(std::string)>;
using DirectoryList = std::vector<std::string>;

class BaseFileSystem {
public:
    virtual ~BaseFileSystem() = default;

    // File Manipulation
    virtual bool        FileExists(std::string_view path)                      const = 0;
    virtual bool        DeleteFile(std::string_view path)                      const = 0;
    virtual bool        RenameFile(std::string_view from, std::string_view to) const = 0;
    virtual std::size_t GetFileSize(std::string_view path)                     const = 0;

    // Directory Manipulation
    virtual bool          DirectoryExists(std::string_view path)                                                const = 0;
    virtual bool          CreateDirectory(std::string_view path, bool recurseParentDir = true)                  const = 0;
    virtual bool          DeleteDirectory(std::string_view path)                                                const = 0;
    virtual DirectoryList ListDirectory(std::string_view path, bool shouldRecurse)                              const = 0;
    virtual void          ListDirectory(std::string_view path, bool shouldRecurse, const FileCallback& onEntry) const = 0;
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