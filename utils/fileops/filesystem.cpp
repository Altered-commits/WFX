// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "posix/filemanip.hpp"
#include "filesystem.hpp"
#include "shared/utils/detection_macro.hpp"

namespace WFX::Utils {

namespace FileSystem {

bool CreateFile(const char* path)
{
    const int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if(fd >= 0) {
        close(fd);
        return true; // Created
    }

    if(errno == EEXIST)
        return true; // Already exists

    return false;
}

bool FileExists(const char* path)
{
    if(!path)
        return false;

    struct stat st {};
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
}

bool DeleteFile(const char* path)
{
    if(!path)
        return false;

    return unlink(path) == 0;
}

bool RenameFile(const char* from, const char* to)
{
    if(!from || !to)
        return false;

    return rename(from, to) == 0;
}

std::size_t GetFileSize(const char* path)
{
    if(!path)
        return 0;

    struct stat st {};
    if(stat(path, &st) == 0 && S_ISREG(st.st_mode))
        return static_cast<std::size_t>(st.st_size);

    return 0;
}

bool GetFileStats(const char* path, FileStats& out)
{
    struct stat st {};
    if(stat(path, &st) != 0)
        return false;

    out.size = st.st_size;

#ifdef WFX_PLATFORM_MACOS
    auto sec = st.st_mtimespec.tv_sec;
    auto nsec = st.st_mtimespec.tv_nsec;
#else
    auto sec = st.st_mtim.tv_sec;
    auto nsec = st.st_mtim.tv_nsec;
#endif

    out.modifiedNs = static_cast<std::int64_t>(sec) * 1'000'000'000LL + static_cast<std::int64_t>(nsec);

    if S_ISREG(st.st_mode)
        out.type = FileType::REG;
    else if S_ISDIR(st.st_mode)
        out.type = FileType::DIR;
    else if S_ISLNK(st.st_mode)
        out.type = FileType::LNK;
    else
        out.type = FileType::OTHER;

    return true;
}

// vvv File Handling vvv
BaseFilePtr OpenFileRead(const char* path, bool inBinaryMode)
{
    // Ignored on posix
    (void)inBinaryMode;

    auto file = std::make_unique<PosixFile>();
    if(!file->OpenRead(path))
        return nullptr;

    return file;
}

BaseFilePtr OpenFileWrite(const char* path, bool inBinaryMode)
{
    // Ignored on posix
    (void)inBinaryMode;

    auto file = std::make_unique<PosixFile>();
    if(!file->OpenWrite(path))
        return nullptr;

    return file;
}

BaseFilePtr OpenFileExisting(WFXFileDescriptor fd, bool fromCache)
{
    if(fd < 0)
        return nullptr;

    struct stat st;
    if(fstat(fd, &st) != 0)
        return nullptr;

    auto file = std::make_unique<PosixFile>();
    file->OpenExisting(fd, st.st_size, fromCache);

    return file;
}

BaseFilePtr OpenFileExisting(WFXFileDescriptor fd, std::size_t size, bool fromCache)
{
    if(fd < 0 || size == 0)
        return nullptr;

    auto file = std::make_unique<PosixFile>();
    file->OpenExisting(fd, size, fromCache);

    return file;
}

// vvv Directory Manipulation vvv
bool DirectoryExists(const char* path)
{
    if(!path)
        return false;

    struct stat st {};
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}

bool CreateDirectory(std::string path, bool recurseParentDir)
{
    if(path.empty())
        return false;

    // Trim trailing slash (except "/")
    if(path.size() > 1 && path.back() == '/')
        path.pop_back();

    // Non-recursive
    if(!recurseParentDir) {
        errno = 0;
        return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
    }

    bool ok = true;
    const char* data = path.c_str();
    const std::size_t len = path.size();

    // Reusable buffer
    std::string tmp;
    tmp.reserve(len);

    std::size_t start = 0;

    // Absolute path handling
    if(data[0] == '/') {
        tmp.push_back('/');
        start = 1;
    }

    for(std::size_t i = start; i <= len; ++i) {
        if(i == len || data[i] == '/') {
            if(i > start) {
                tmp.append(data + start, i - start);

                errno = 0;
                if(mkdir(tmp.c_str(), 0755) != 0 && errno != EEXIST) {
                    ok = false;
                    break;
                }
            }

            if(i < len)
                tmp.push_back('/');

            start = i + 1;
        }
    }

    return ok;
}

bool DeleteDirectory(const char* path)
{
    if(!path)
        return false;

    return rmdir(path) == 0;
}

// --- Forward declare helper function
void ListDirectoryImpl(std::string& path, bool shouldRecurse, const FileCallback& callback);

DirectoryList ListDirectory(std::string path, bool shouldRecurse)
{
    DirectoryList result;
    ListDirectoryImpl(path, shouldRecurse, [&](std::string p) { result.emplace_back(std::move(p)); });
    return result;
}

void ListDirectory(std::string path, bool shouldRecurse, const FileCallback& callback)
{
    ListDirectoryImpl(path, shouldRecurse, callback);
}

// vvv Helper Functions vvv
void ListDirectoryImpl(std::string& path, bool shouldRecurse, const FileCallback& callback)
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
        struct stat st {};

        if(lstat(fullPath.c_str(), &st) == 0)
            isDir = S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);

        if(shouldRecurse && isDir)
            ListDirectoryImpl(fullPath, true, callback);
    }
    closedir(dir);
}

// vvv Path Queries vvv
std::string GetCurrentPath()
{
    char buf[PATH_MAX];

    if(!getcwd(buf, sizeof(buf)))
        return "";

    return std::string(buf);
}

} // namespace FileSystem

} // namespace WFX::Utils
