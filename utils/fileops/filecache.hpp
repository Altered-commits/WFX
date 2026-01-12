#ifndef WFX_UTILS_FILE_CACHE_HPP
#define WFX_UTILS_FILE_CACHE_HPP

#include "utils/common/file.hpp"
#include <list>
#include <string>
#include <unordered_map>
#include <cstdint>

namespace WFX::Utils {

struct CacheEntry {
    int           fd;                            // Actual file descriptor
    std::uint64_t freq;                          // Access frequency
    off_t         fileSize;                      // File size in bytes
    std::list<std::string>::iterator bucketIter; // Position in the frequency bucket list
};

class FileCache final {
public:
    static FileCache& GetInstance();
    void Init(std::size_t capacity);

public:
    std::pair<WFXFileDescriptor, WFXFileSize> GetFileDesc(const std::string& path);

private:
    FileCache() = default;
    ~FileCache();

    // No need for copy / move semantics
    FileCache(const FileCache&)            = delete;
    FileCache(FileCache&&)                 = delete;
    FileCache& operator=(const FileCache&) = delete;
    FileCache& operator=(FileCache&&)      = delete;

private: // Helper Functions
    void Touch(const std::string& key);
    void Insert(const std::string& key, WFXFileDescriptor fd, WFXFileSize size);
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