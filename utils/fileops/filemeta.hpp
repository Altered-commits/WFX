#ifndef WFX_UTILS_FILEMETA_HPP
#define WFX_UTILS_FILEMETA_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include <unordered_map>
#include <tuple>

namespace WFX::Utils {

using FileMetadata = std::tuple<
    std::uint64_t, // Size
    std::uint64_t, // Modified time
    std::string    // Hash
>;

// For ease of use :)
using FileBuffer  = std::vector<char>;
using FileMetaMap = std::unordered_map<std::string, FileMetadata>;

enum class FileMetaStatus {
    SUCCESS,
    NOT_FOUND,
    TOO_LARGE,
    TOO_MANY_ENTRIES,
    CORRUPTED,
    IO_ERROR
};

class FileMeta {
public:
    FileMeta(std::string filePath);
    ~FileMeta() = default;

public: // Main functions
    FileMetaStatus Load();
    FileMetaStatus Save() const;

    FileMetadata* Get(const std::string&);
    void          Set(std::string file, FileMetadata meta);

    void Erase(const std::string& file);
    void Clear();

private: // Helper functions
    std::size_t FindSeparator(const FileBuffer& buffer, std::size_t idx);

private: // Constexpr stuff
    constexpr static std::size_t allocThreshold_ = 1024 * 1024; // 1 MB
    constexpr static std::size_t entryThreshold_ = 5000;        // 5000 entries
    constexpr static std::size_t minimumEntries_ = 32;          // 32 entries expected at minimum
    constexpr static std::size_t lineSize_       = 120;         // 120 bytes per metadata
    constexpr static std::size_t bufferEnd_      = std::numeric_limits<std::size_t>::max();
    constexpr static char        fieldSeparator_ = '\x1F';

private: // Storage
    std::string filePath_;
    FileMetaMap meta_;
};

} // namespace WFX::Utils

#endif // WFX_UTILS_FILEMETA_HPP