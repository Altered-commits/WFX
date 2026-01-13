#include "filecache.hpp"
#include "utils/logger/logger.hpp"

// For windows, filecache.hpp already includes windows.h anyways
#ifdef _WIN32
    #define CloseFile(fd) CloseHandle(fd)
#else
    #include <sys/stat.h>
    #include <sys/resource.h>
    #include <fcntl.h>
    #include <unistd.h>

    #define CloseFile(fd) close(fd)
#endif

#include <cassert>

namespace WFX::Utils {

// vvv Constructor & Destructor vvvv
FileCache& FileCache::GetInstance()
{
    static FileCache fileCache;
    return fileCache;
}

FileCache::~FileCache()
{
    for(auto& pair : entries_)
        CloseFile(pair.second.fd);

    if(!entries_.empty())
        Logger::GetInstance().Info("[FileCache]: Closed all cached file descriptors successfully");
}

void FileCache::Init(std::size_t capacity)
{
    minFreq_ = 0;
    std::size_t safe = capacity;

#ifndef _WIN32
    // Leave room for sockets + other fds on Linux/Unix
    struct rlimit rl;
    if(getrlimit(RLIMIT_NOFILE, &rl) == 0)
        safe = rl.rlim_cur / 2;
#endif
    
    capacity_ = std::min(capacity, safe);
}

// vvv User Functions vvv
std::pair<WFXFileDescriptor, WFXFileSize> FileCache::GetFileDesc(const std::string& path)
{
    auto it = entries_.find(path);
    if(it != entries_.end()) {
        Touch(it->first);
        return {it->second.fd, it->second.fileSize};
    }

    WFXFileDescriptor fd   = 0;
    WFXFileSize       size = 0;

#ifdef _WIN32
    fd = CreateFileA(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if(fd == WFX_INVALID_FILE)
        return {WFX_INVALID_FILE, 0};

    LARGE_INTEGER fsize;
    if(!GetFileSizeEx(fd, &fsize)) {
        CloseHandle(fd);
        return {WFX_INVALID_FILE, 0};
    }

    size = static_cast<std::uint64_t>(fsize.QuadPart);
#else
    fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if(fd < 0)
        return {WFX_INVALID_FILE, 0};

    struct stat st;
    if(fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return {WFX_INVALID_FILE, 0};
    }

    size = st.st_size;
#endif

    Insert(path, fd, size);
    return {fd, size};
}

// vvv Helper Functions vvv
void FileCache::Touch(const std::string& key)
{
    auto &entry = entries_[key];
    int oldFreq = entry.freq;
    int newFreq = oldFreq + 1;
    
    entry.freq = newFreq;

    // Remove from old bucket
    freqBuckets_[oldFreq].erase(entry.bucketIter);
    if(freqBuckets_[oldFreq].empty()) {
        freqBuckets_.erase(oldFreq);
        if(minFreq_ == oldFreq)
            minFreq_ = newFreq;
    }

    // Insert into new bucket at front (most recently used among same freq)
    freqBuckets_[newFreq].push_front(key);
    entry.bucketIter = freqBuckets_[newFreq].begin();
}

void FileCache::Insert(const std::string& key, WFXFileDescriptor fd, WFXFileSize size)
{
    if(entries_.size() >= capacity_)
        Evict();

    // Insert with freq = 1
    freqBuckets_[1].push_front(key);
    entries_[key] = {fd, 1, size, freqBuckets_[1].begin()};
    minFreq_ = 1;
}

void FileCache::Evict()
{
    assert(!freqBuckets_.empty());

    // Evict from minFreq bucket, oldest entry (back of list)
    auto &bucket = freqBuckets_[minFreq_];
    std::string keyToEvict = bucket.back();
    bucket.pop_back();

    WFXFileDescriptor fd = entries_[keyToEvict].fd;
    CloseFile(fd); // Close FD

    entries_.erase(keyToEvict);

    // minFreq_ will be reset on next insertion
    if(bucket.empty())
        freqBuckets_.erase(minFreq_);
}

} // namespace WFX::Utils