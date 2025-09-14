#ifndef WFX_LINUX_FILESYSTEM_HPP
#define WFX_LINUX_FILESYSTEM_HPP

#include "utils/filesystem/filesystem.hpp"

namespace WFX::OSSpecific {

using namespace WFX::Utils; // For 'FileSystem'

class LinuxFileSystem : public BaseFileSystem {
public:
    // File Manipulation
    bool        FileExists(const char* path)                 const override;
    bool        DeleteFile(const char* path)                 const override;
    bool        RenameFile(const char* from, const char* to) const override;
    std::size_t GetFileSize(const char* path)                const override;

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