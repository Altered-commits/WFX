#include "filemanip.hpp"

namespace WFX::Utils {

MacOSFile::~MacOSFile()
{
    Close();
}

void MacOSFile::Close()
{
    if(fd_ >= 0 && !existing_) {
        ::close(fd_);
        fd_ = -1;
        size_ = 0;
        existing_ = false;
        cached_ = false;
    }
}

std::int64_t MacOSFile::Read(void* buffer, std::size_t bytes)
{
    if(cached_ || fd_ < 0)
        return -1;

    ssize_t n = ::read(fd_, buffer, bytes);
    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

std::int64_t MacOSFile::Write(const void* buffer, std::size_t bytes)
{
    if(cached_ || fd_ < 0)
        return -1;

    ssize_t n = ::write(fd_, buffer, bytes);
    if(n > 0)
        size_ += n;

    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

std::int64_t MacOSFile::ReadAt(void* buffer, std::size_t bytes, std::size_t offset)
{
    if(fd_ < 0)
        return -1;

    ssize_t n = ::pread(fd_, buffer, bytes, static_cast<off_t>(offset));
    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

std::int64_t MacOSFile::WriteAt(const void* buffer, std::size_t bytes, std::size_t offset)
{
    if(cached_ || fd_ < 0)
        return -1;

    ssize_t n = ::pwrite(fd_, buffer, bytes, static_cast<off_t>(offset));
    if(n > 0 && (offset + n > size_))
        size_ = offset + n;

    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

bool MacOSFile::Seek(std::size_t offset)
{
    if(cached_ || fd_ < 0)
        return false;

    off_t ret = ::lseek(fd_, static_cast<off_t>(offset), SEEK_SET);
    return ret != static_cast<off_t>(-1);
}

std::int64_t MacOSFile::Tell() const
{
    if(fd_ < 0)
        return 0;

    off_t ret = ::lseek(fd_, 0, SEEK_CUR);
    return ret < 0 ? -1 : static_cast<std::int64_t>(ret);
}

std::size_t MacOSFile::Size() const
{
    return size_;
}

bool MacOSFile::IsOpen() const
{
    return fd_ >= 0;
}

bool MacOSFile::OpenRead(const char* path)
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

bool MacOSFile::OpenWrite(const char* path)
{
    Close();

    fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(fd_ < 0)
        return false;

    size_ = 0;
    return true;
}

void MacOSFile::OpenExisting(int fd, std::size_t size, bool cached)
{
    fd_ = fd;
    existing_ = true;
    cached_ = cached;
    size_ = size;
}

} // namespace WFX::Utils
