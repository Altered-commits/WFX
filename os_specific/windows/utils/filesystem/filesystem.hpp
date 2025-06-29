#ifndef WFX_WINDOWS_FILESYSTEM_HPP
#define WFX_WINDOWS_FILESYSTEM_HPP

#include "utils/filesystem/filesystem.hpp"

namespace WFX::OSSpecific {

using namespace WFX::Utils; // For 'FileSystem'

class WinFileSystem : public BaseFileSystem {
public:
    // File Manipulation
    bool FileExists(std::string_view path)                      const override;
    bool DeleteFile(std::string_view path)                      const override;
    bool RenameFile(std::string_view from, std::string_view to) const override;
    std::size_t GetFileSize(std::string_view path)              const override;

    // Directory Manipulation
    bool DirectoryExists(std::string_view path)                   const override;
    bool CreateDirectory(std::string_view path)                   const override;
    bool DeleteDirectory(std::string_view path)                   const override;
    std::vector<std::string> ListDirectory(std::string_view path) const override;
};

} // namespace WFX::OSSpecific


#endif // WFX_WINDOWS_FILESYSTEM_HPP