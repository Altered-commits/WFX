#if defined(_WIN32)
    #include "windows/filemanip.hpp"
#else
    #include "linux/filemanip.hpp"
#endif

namespace WFX::Utils {

namespace FileSystem {

bool CreateFile(const char *path)
{
#ifdef _WIN32
    ...
#else
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if(fd >= 0) {
        close(fd);
        return true; // Created
    }

    if(errno == EEXIST)
        return true; // Already exists

    return false;
#endif
}

bool FileExists(const char* path)
{
    if(!path)
        return false;
#ifdef _WIN32
    DWORD attrib = GetFileAttributesA(path.data());
    return (attrib != INVALID_FILE_ATTRIBUTES) && !(attrib & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st{};
    return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
#endif
}

bool DeleteFile(const char* path)
{
    if(!path)
        return false;
#ifdef _WIN32
    return DeleteFileA(path.data()) != 0;
#else
    return unlink(path) == 0;
#endif
}

bool RenameFile(const char* from, const char* to)
{
    if(!from || !to)
        return false;
#ifdef _WIN32
    return MoveFileA(from.data(), to.data()) != 0;
#else
    return rename(from, to) == 0;
#endif
}

std::size_t GetFileSize(const char* path)
{
    if(!path)
        return 0;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    if(!GetFileAttributesExA(path.data(), GetFileExInfoStandard, &fileInfo))
        return 0;

    LARGE_INTEGER size;
    size.HighPart = fileInfo.nFileSizeHigh;
    size.LowPart  = fileInfo.nFileSizeLow;
    
    return static_cast<std::size_t>(size.QuadPart);
#else
    struct stat st{};
    if(stat(path, &st) == 0 && S_ISREG(st.st_mode))
        return static_cast<std::size_t>(st.st_size);

    return 0;
#endif
}

// vvv File Handling vvv
BaseFilePtr OpenFileRead(const char* path, bool inBinaryMode)
{
#ifdef _WIN32
    ...
#else
    // Ignored in linux
    (void)inBinaryMode;

    auto file = std::make_unique<LinuxFile>();
    if(!file->OpenRead(path))
        return nullptr;

    return file;
#endif
}

BaseFilePtr OpenFileWrite(const char* path, bool inBinaryMode)
{
#ifdef _WIN32
    ...
#else
    // Ignored in linux
    (void)inBinaryMode;

    auto file = std::make_unique<LinuxFile>();
    if(!file->OpenWrite(path))
        return nullptr;

    return file;
#endif
}

BaseFilePtr OpenFileExisting(WFXFileDescriptor fd, bool fromCache)
{
#ifdef _WIN32
    ...
#else
    if(fd < 0)
        return nullptr;

    struct stat st;
    if(fstat(fd, &st) != 0)
        return nullptr;

    auto file = std::make_unique<LinuxFile>();
    file->OpenExisting(fd, st.st_size, fromCache);

    return file;
#endif
}

BaseFilePtr OpenFileExisting(WFXFileDescriptor fd, std::size_t size, bool fromCache)
{
#ifdef _WIN32
    ...
#else
    if(fd < 0 || size == 0)
        return nullptr;

    auto file = std::make_unique<LinuxFile>();
    file->OpenExisting(fd, size, fromCache);

    return file;
#endif
}

// vvv Directory Manipulation vvv
bool DirectoryExists(const char* path)
{
    if(!path)
        return false;
#ifdef _WIN32
    DWORD attrib = GetFileAttributesA(path.data());
    return (attrib != INVALID_FILE_ATTRIBUTES) && (attrib & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st{};
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
#endif
}

bool CreateDirectory(std::string path, bool recurseParentDir)
{
#ifdef _WIN32
    ...
#else
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
    std::size_t len  = path.size();

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
#endif
}

bool DeleteDirectory(const char* path)
{
    if(!path)
        return false;
#ifdef _WIN32
    return RemoveDirectoryA(path.data()) != 0;
#else
    return rmdir(path) == 0;
#endif
}

// --- Forward declare helper function
void ListDirectoryImpl(std::string& path, bool shouldRecurse, const FileCallback& callback);

DirectoryList ListDirectory(std::string path, bool shouldRecurse)
{
    DirectoryList result;
    ListDirectoryImpl(path, shouldRecurse, [&](std::string p) {
        result.emplace_back(std::move(p));
    });
    return result;
}

void ListDirectory(std::string path, bool shouldRecurse, const FileCallback& callback)
{
    ListDirectoryImpl(path, shouldRecurse, callback);
}

// vvv Helper Functions vvv
void ListDirectoryImpl(std::string& path, bool shouldRecurse, const FileCallback& callback)
{
#ifdef _WIN32
    ...
#else
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
#endif
}

} // namespace Filesystem

} // namespace WFX::Utils