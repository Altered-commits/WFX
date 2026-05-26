#ifndef WFX_UTILS_FILEMETA_HPP
#define WFX_UTILS_FILEMETA_HPP

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <unordered_map>

namespace WFX::Utils {

struct FileMetadata {
public: // Constructors
    FileMetadata() = default;

    FileMetadata(std::int64_t mt, std::string&& hs) : modifiedTime(mt), hash(std::move(hs))
    {}

public: // Main Functions
    template <typename T> void Push(const T& v)
    {
        static_assert(std::is_trivially_copyable_v<T>, "FileMetadata.Push<T>: T must be trivially copyable");

        const char* p = reinterpret_cast<const char*>(&v);
        userData.insert(userData.end(), p, p + sizeof(T));
    }

    template <typename T> T Pop(std::size_t& offset) const
    {
        static_assert(std::is_trivially_copyable_v<T>, "FileMetadata.Pop<T>: T must be trivially copyable");

        T v;
        std::memcpy(&v, userData.data() + offset, sizeof(T));
        offset += sizeof(T);

        return v;
    }

    // vvv 'std::string' overload vvv
    void Push(const std::string& s)
    {
        std::uint32_t len = static_cast<std::uint32_t>(s.size());
        Push(len);
        userData.insert(userData.end(), s.begin(), s.end());
    }

    std::string PopString(std::size_t& offset) const
    {
        std::uint32_t len = Pop<std::uint32_t>(offset);
        std::string s(userData.data() + offset, userData.data() + offset + len);
        offset += len;
        return s;
    }

public: // Metadata
    std::int64_t modifiedTime{0};
    std::string hash{};
    std::vector<char> userData; // Any extra data which user wants to store (Must be pre-serialized)
    bool hit{false};            // Only for runtime usage
};

// For ease of use :)
using FileBuffer = std::vector<char>;
using FileMetaMap = std::unordered_map<std::string, FileMetadata>;

enum class FileMetaStatus { SUCCESS, NOT_FOUND, TOO_LARGE, TOO_MANY_ENTRIES, CORRUPTED, IO_ERROR };

class FileMeta {
public:
    FileMeta(std::string filePath);
    ~FileMeta() = default;

public: // Main functions
    FileMetaStatus Load();
    FileMetaStatus Save() const;

    FileMetadata* Get(const std::string& file, bool processHit = true);
    void Set(std::string file, FileMetadata meta);

    void Erase(const std::string& file);
    void Clear();

private: // Helper functions
    std::size_t FindSeparator(const FileBuffer& buffer, std::size_t idx);

private:                                                        // Constexpr stuff
    constexpr static std::size_t ALLOC_THRESHOLD = 1024 * 1024; // 1 MB
    constexpr static std::size_t ENTRY_THRESHOLD = 5000;        // 5000 entries
    constexpr static std::size_t MINIMUM_ENTRIES = 32;          // 32 entries expected at minimum (hint)
    constexpr static std::size_t LINE_SIZE = 120;               // 120 bytes per metadata (hint)
    constexpr static std::size_t BUFFER_END = std::numeric_limits<std::size_t>::max();
    constexpr static char FIELD_SEPARATOR = '\x1F';

private: // Storage
    std::string filePath_;
    FileMetaMap meta_;
};

} // namespace WFX::Utils

#endif // WFX_UTILS_FILEMETA_HPP