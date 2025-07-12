#ifndef WFX_UTILS_UUID_HPP
#define WFX_UTILS_UUID_HPP

#include <cstdint>
#include <cstring>
#include <string>

namespace WFX::Utils {

struct UUID {
    UUID() { std::memset(bytes, 0, 16); }
    
    static bool FromString(std::string_view str, UUID& out);
    std::string ToString() const;

    // Operators
    bool operator==(const UUID& other) const;

    std::uint8_t bytes[16];
};

} // namespace WFX::Utils

// std::hash specialization so i can use it in std::unordered_map stuff
namespace std {
    template<>
    struct hash<WFX::Utils::UUID> {
        size_t operator()(const WFX::Utils::UUID& uuid) const
        {
            std::uint64_t hi, lo;
            std::memcpy(&lo, uuid.bytes, sizeof(std::uint64_t));
            std::memcpy(&hi, uuid.bytes + 8, sizeof(std::uint64_t));
            
            hi ^= lo >> 33;
            hi *= 0xff51afd7ed558ccdULL;
            hi ^= hi >> 33;
            hi *= 0xc4ceb9fe1a85ec53ULL;
            hi ^= hi >> 33;
            return static_cast<size_t>(hi);
        }
    };
}

#endif // WFX_UTILS_UUID_HPP