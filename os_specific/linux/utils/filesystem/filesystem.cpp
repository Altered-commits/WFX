#include "filesystem.hpp"

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace WFX::OSSpecific {

// vvv File Manipulation vvv
bool LinuxFileSystem::FileExists(const char* path) const
{
    struct stat st{};
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
}

bool LinuxFileSystem::DeleteFile(const char* path) const
{
    return unlink(path) == 0;
}

bool LinuxFileSystem::RenameFile(const char* from, const char* to) const
{
    return rename(from, to) == 0;
}

std::size_t LinuxFileSystem::GetFileSize(const char* path) const
{
    struct stat st{};
    if(stat(path, &st) == 0 && S_ISREG(st.st_mode))
        return static_cast<std::size_t>(st.st_size);

    return 0;
}

// vvv Directory Manipulation vvv
bool LinuxFileSystem::DirectoryExists(const char* path) const
{
    struct stat st{};
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}

bool LinuxFileSystem::CreateDirectory(std::string path, bool recurseParentDir) const
{
    if(path.empty())
        return false;

    // Trim trailing slash if present
    std::size_t len = path.size();
    if(len > 1 && path[len - 1] == '/')
        len--;

    // If not recursive, just try once
    if(!recurseParentDir) {
        std::string tmp(path.substr(0, len)); // Small one-off for syscall
        return mkdir(tmp.c_str(), 0755) == 0 || errno == EEXIST;
    }

    bool ok = true;
    std::size_t pos = 0;

    // Walk through path and mkdir each subpath
    for(std::size_t i = 1; i <= len; ++i) {
        if(i == len || path[i] == '/') {
            std::size_t subLen = i;
            if(subLen == 0) continue;

            std::string tmp(path.data(), subLen);
            if(mkdir(tmp.c_str(), 0755) != 0 && errno != EEXIST) {
                ok = false;
                break;
            }
        }
    }

    return ok;
}

bool LinuxFileSystem::DeleteDirectory(const char* path) const
{
    return rmdir(path) == 0;
}

DirectoryList LinuxFileSystem::ListDirectory(std::string path, bool shouldRecurse) const
{
    DirectoryList result;
    ListDirectoryImpl(path, shouldRecurse, [&](std::string p) {
        result.push_back(std::move(p));
    });
    return result;
}

void LinuxFileSystem::ListDirectory(std::string path, bool shouldRecurse, const FileCallback& callback) const
{
    ListDirectoryImpl(path, shouldRecurse, callback);
}

// vvv Helper Functions vvv
void LinuxFileSystem::ListDirectoryImpl(std::string& path, bool shouldRecurse, const FileCallback& callback) const
{
    DIR* dir = opendir(path.data());
    if(!dir)
        return;

    struct dirent* entry;
    while((entry = readdir(dir)) != nullptr) {
        // skip . and ..
        if(std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
            continue;

        std::string fullPath = path + "/" + entry->d_name;
        callback(fullPath);

        bool isDir = false;
        struct stat st{};
        
        if(lstat(fullPath.c_str(), &st) == 0)
            isDir = S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);

        if(shouldRecurse && isDir)
            ListDirectoryImpl(fullPath, true, callback);
    }
    closedir(dir);
}

} // namespace WFX::OSSpecific