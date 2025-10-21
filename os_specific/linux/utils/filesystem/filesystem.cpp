#include "filesystem.hpp"

#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace WFX::OSSpecific {

// vvv File Wrapper vvv
//  --- Cleanup Functions ---
LinuxFile::~LinuxFile()
{
    Close();
}

void LinuxFile::Close()
{
    // If u open from existing, u cannot close it like this
    if(fd_ >= 0 && !existing_) {
        ::close(fd_);
        fd_ = -1;
        existing_ = false;
    }
}

//  --- File Operations ---
std::int64_t LinuxFile::Read(void* buffer, std::size_t bytes)
{
    if(fd_ < 0)
        return 0;
    
    ssize_t n = ::read(fd_, buffer, bytes);
    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

std::int64_t LinuxFile::Write(const void* buffer, std::size_t bytes)
{
    if(fd_ < 0)
        return 0;

    ssize_t n = ::write(fd_, buffer, bytes);
    if(n > 0)
        size_ += n;
    
    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

std::int64_t LinuxFile::ReadAt(void *buffer, std::size_t bytes, std::size_t offset)
{
    if(fd_ < 0)
        return 0;

    ssize_t n = ::pread(fd_, buffer, bytes, static_cast<off_t>(offset));
    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

std::int64_t LinuxFile::WriteAt(const void *buffer, std::size_t bytes, std::size_t offset)
{
    if(fd_ < 0)
        return 0;

    ssize_t n = ::pwrite(fd_, buffer, bytes, static_cast<off_t>(offset));
    if(n > 0 && (offset + n > size_))
        size_ = offset + n;  // Update file size if we wrote past previous end

    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

bool LinuxFile::Seek(std::size_t offset)
{
    if(fd_ < 0)
        return false;
    
    off_t ret = ::lseek(fd_, static_cast<off_t>(offset), SEEK_SET);
    return ret != static_cast<off_t>(-1);
}

std::int64_t LinuxFile::Tell() const
{
    if(fd_ < 0)
        return 0;
    
    off_t ret = ::lseek(fd_, 0, SEEK_CUR);
    return ret < 0 ? -1 : static_cast<std::int64_t>(ret);
}

//  --- Utility Functions ---
std::size_t LinuxFile::Size() const
{
    return size_;
}

bool LinuxFile::IsOpen() const
{
    return fd_ >= 0;
}

//  --- Helper Functions ---
bool LinuxFile::OpenRead(const char* path)
{
    Close();
    fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
    if(fd_ < 0)
        return false;

    struct stat st;
    if(fstat(fd_, &st) != 0) {
        Close();
        return false;
    }

    size_ = st.st_size;
    return true;
}

bool LinuxFile::OpenWrite(const char* path)
{
    Close();

    fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(fd_ < 0)
        return false;
    
    size_ = 0;
    return true;
}

void LinuxFile::OpenExisting(int fd, std::size_t size)
{
    fd_       = fd;
    existing_ = true;
    size_     = size;
}

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

// vvv File Handling vvv
BaseFilePtr LinuxFileSystem::OpenFileRead(const char* path, bool inBinaryMode)
{
    // Ignored in linux
    (void)inBinaryMode;

    auto file = std::make_unique<LinuxFile>();
    if(!file->OpenRead(path))
        return nullptr;

    return file;
}

BaseFilePtr LinuxFileSystem::OpenFileWrite(const char* path, bool inBinaryMode)
{
    // Ignored in linux
    (void)inBinaryMode;

    auto file = std::make_unique<LinuxFile>();
    if(!file->OpenWrite(path))
        return nullptr;

    return file;
}

BaseFilePtr LinuxFileSystem::OpenFileExisting(WFXFileDescriptor fd)
{
    if(fd < 0)
        return nullptr;

    struct stat st;
    if(fstat(fd, &st) != 0)
        return nullptr;

    auto file = std::make_unique<LinuxFile>();
    file->OpenExisting(fd, st.st_size);

    return file;
}

BaseFilePtr LinuxFileSystem::OpenFileExisting(WFXFileDescriptor fd, std::size_t size)
{
    if(fd < 0 || size == 0)
        return nullptr;

    auto file = std::make_unique<LinuxFile>();
    file->OpenExisting(fd, size);

    return file;
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