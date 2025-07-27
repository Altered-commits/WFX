#ifndef WFX_WINDOWS_FILESYSTEM_HPP
#define WFX_WINDOWS_FILESYSTEM_HPP

#include "utils/filesystem/filesystem.hpp"

namespace WFX::OSSpecific {

using namespace WFX::Utils; // For 'FileSystem'

class WinFileSystem : public BaseFileSystem {
public:
    // File Manipulation
    bool        FileExists(std::string_view path)                      const override;
    bool        DeleteFile(std::string_view path)                      const override;
    bool        RenameFile(std::string_view from, std::string_view to) const override;
    std::size_t GetFileSize(std::string_view path)                     const override;

    // Directory Manipulation
    bool          DirectoryExists(std::string_view path)                                                 const override;
    bool          CreateDirectory(std::string_view path, bool recurseParentDir)                          const override;
    bool          DeleteDirectory(std::string_view path)                                                 const override;
    DirectoryList ListDirectory(std::string_view path, bool shouldRecurse)                               const override;
    void          ListDirectory(std::string_view path, bool shouldRecurse, const FileCallback& callback) const override;

private: // Helper functions
    void ListDirectoryImpl(std::string_view path, bool shouldRecurse, const FileCallback& callback) const;
};

} // namespace WFX::OSSpecific


#endif // WFX_WINDOWS_FILESYSTEM_HPP