#ifndef WFX_UTILS_FILE_CACHE_HPP
#define WFX_UTILS_FILE_CACHE_HPP

#include <list>
#include <string>
#include <unordered_map>
#include <cstdint>

#ifdef _WIN32
    #include <windows.h>
    using FileDescriptor = HANDLE;
    using FileSize       = std::uint64_t;
#else
    #include <sys/types.h>
    using FileDescriptor = int;
    using FileSize       = off_t;
#endif

namespace WFX::Utils {

struct CacheEntry {
    int           fd;                            // Actual file descriptor
    std::uint64_t freq;                          // Access frequency
    off_t         fileSize;                      // File size in bytes
    std::list<std::string>::iterator bucketIter; // Position in the frequency bucket list
};

class FileCache final {
public:
    FileCache(std::size_t capacity);
    ~FileCache();

public:
    std::pair<FileDescriptor, FileSize> GetFileDesc(const std::string& path);

private:
    // No need for copy / move semantics
    FileCache(const FileCache&)            = delete;
    FileCache(FileCache&&)                 = delete;
    FileCache& operator=(const FileCache&) = delete;
    FileCache& operator=(FileCache&&)      = delete;

private: // Helper Functions
    void Touch(const std::string& key);
    void Insert(const std::string& key, FileDescriptor fd, FileSize size);
    void Evict();

private:
    std::size_t   capacity_;
    std::uint64_t minFreq_;

    // Key -> CacheEntry
    std::unordered_map<std::string, CacheEntry> entries_;

    // Frequency -> list of keys (for LFU eviction)
    std::unordered_map<std::uint64_t, std::list<std::string>> freqBuckets_;
};

} // namespace WFX::Utils

#endif // WFX_UTILS_FILE_CACHE_HPP