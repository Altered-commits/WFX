#ifndef WFX_UTILS_FILESYSTEM_HPP
#define WFX_UTILS_FILESYSTEM_HPP

#include <string>
#include <vector>
#include <cstddef>

namespace WFX::Utils {

class BaseFileSystem {
public:
    virtual ~BaseFileSystem() = default;

    // File Manipulation
    virtual bool        FileExists(std::string_view path)                      const = 0;
    virtual bool        DeleteFile(std::string_view path)                      const = 0;
    virtual bool        RenameFile(std::string_view from, std::string_view to) const = 0;
    virtual std::size_t GetFileSize(std::string_view path)                     const = 0;

    // Directory Manipulation
    virtual bool                     DirectoryExists(std::string_view path) const = 0;
    virtual bool                     CreateDirectory(std::string_view path) const = 0;
    virtual bool                     DeleteDirectory(std::string_view path) const = 0;
    virtual std::vector<std::string> ListDirectory(std::string_view path)   const = 0;
};

// MAIN SHIT: Returns singleton reference to stuff needed for File / Directory manipulation
// Also using this as namespace lmao
struct FileSystem {
    static BaseFileSystem& GetFileSystem();
};

} // namespace WFX::Utils

#endif // WFX_UTILS_FILESYSTEM_HPP