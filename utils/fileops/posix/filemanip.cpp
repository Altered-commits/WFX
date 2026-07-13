// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "filemanip.hpp"

namespace WFX::Utils {

// vvv File Wrapper vvv
//  --- Cleanup Functions ---
PosixFile::~PosixFile()
{
    // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.VirtualCall) - PosixFile is a leaf class, no further overrides
    Close();
}

void PosixFile::Close()
{
    // If u open from existing, u cannot close it like this
    if(fd_ >= 0 && !existing_) {
        ::close(fd_);
        fd_ = -1;
        size_ = 0;
        existing_ = false;
        cached_ = false;
    }
}

//  --- File Operations ---
std::int64_t PosixFile::Read(void* buffer, std::size_t bytes)
{
    if(cached_ || fd_ < 0)
        return -1;

    const ssize_t n = ::read(fd_, buffer, bytes);
    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

std::int64_t PosixFile::Write(const void* buffer, std::size_t bytes)
{
    if(cached_ || fd_ < 0)
        return -1;

    const ssize_t n = ::write(fd_, buffer, bytes);
    if(n > 0)
        size_ += n;

    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

std::int64_t PosixFile::ReadAt(void* buffer, std::size_t bytes, std::size_t offset)
{
    if(fd_ < 0)
        return -1;

    const ssize_t n = ::pread(fd_, buffer, bytes, static_cast<off_t>(offset));
    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

std::int64_t PosixFile::WriteAt(const void* buffer, std::size_t bytes, std::size_t offset)
{
    if(cached_ || fd_ < 0)
        return -1;

    const ssize_t n = ::pwrite(fd_, buffer, bytes, static_cast<off_t>(offset));
    if(n > 0 && (offset + n > size_))
        size_ = offset + n; // Update file size if we wrote past previous end

    return n < 0 ? -1 : static_cast<std::int64_t>(n);
}

bool PosixFile::Seek(std::size_t offset)
{
    if(cached_ || fd_ < 0)
        return false;

    const off_t ret = ::lseek(fd_, static_cast<off_t>(offset), SEEK_SET);
    return ret != static_cast<off_t>(-1);
}

std::int64_t PosixFile::Tell() const
{
    if(fd_ < 0)
        return 0;

    const off_t ret = ::lseek(fd_, 0, SEEK_CUR);
    return ret < 0 ? -1 : static_cast<std::int64_t>(ret);
}

//  --- Utility Functions ---
std::size_t PosixFile::Size() const
{
    return size_;
}

bool PosixFile::IsOpen() const
{
    return fd_ >= 0;
}

//  --- Helper Functions ---
bool PosixFile::OpenRead(const char* path)
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

bool PosixFile::OpenWrite(const char* path)
{
    Close();

    fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(fd_ < 0)
        return false;

    size_ = 0;
    return true;
}

void PosixFile::OpenExisting(int fd, std::size_t size, bool cached)
{
    fd_ = fd;
    existing_ = true;
    cached_ = cached;
    size_ = size;
}

} // namespace WFX::Utils