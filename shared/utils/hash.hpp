#ifndef WFX_SHARED_UTILS_HASH_HPP
#define WFX_SHARED_UTILS_HASH_HPP

#include "shared/abis/string_view.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace WFX::Shared {

namespace Hasher {

inline constexpr std::uint64_t Fnv1a(const std::uint8_t* data, std::uint64_t len) noexcept
{
    constexpr std::uint64_t prime  = 1099511628211ULL;
    constexpr std::uint64_t offset = 14695981039346656037ULL;

    std::uint64_t hash = offset;
    const std::uint8_t* end = data + len;
    while(data < end) {
        hash ^= *data++;
        hash *= prime;
    }
    return hash;
}

inline std::uint64_t Fnv1a(StringView str) noexcept
{
    return Fnv1a(
        reinterpret_cast<const std::uint8_t*>(str.Data()),
        static_cast<std::uint64_t>(str.Size())
    );
}

inline constexpr std::uint64_t Fnv1aCaseInsensitive(const std::uint8_t* data, std::uint64_t len) noexcept
{
    constexpr std::uint64_t prime  = 1099511628211ULL;
    constexpr std::uint64_t offset = 14695981039346656037ULL;

    std::uint64_t hash = offset;
    const std::uint8_t* end = data + len;

    while(data < end) {
        std::uint8_t c = *data++;
        hash ^= static_cast<std::uint8_t>((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
        hash *= prime;
    }

    return hash;
}

inline std::uint64_t Fnv1aCaseInsensitive(StringView str) noexcept
{
    return Fnv1aCaseInsensitive(
        reinterpret_cast<const std::uint8_t*>(str.Data()),
        static_cast<std::uint64_t>(str.Size())
    );
}

inline constexpr std::uint32_t Murmur3Mix32(std::uint32_t h) noexcept
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

inline constexpr std::uint64_t Murmur3Mix64(std::uint64_t h) noexcept
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

inline constexpr std::uint64_t HashCombine(std::uint64_t a, std::uint64_t b) noexcept
{
    // Based on boost::hash_combine, 64-bit
    a ^= b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2);
    return a;
}

// Hash any trivially copyable value by its raw bytes
template<typename T>
inline std::uint64_t HashValue(const T& val) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>, "HashValue requires trivially copyable type");

    return Fnv1a(
        reinterpret_cast<const std::uint8_t*>(&val),
        sizeof(T)
    );
}

inline constexpr std::uint32_t HashInt32(std::uint32_t x) noexcept
{
    return Murmur3Mix32(x);
}

inline constexpr std::uint64_t HashInt64(std::uint64_t x) noexcept
{
    return Murmur3Mix64(x);
}

inline constexpr std::uint32_t Djb2(const char* data, std::size_t len) noexcept
{
    std::uint32_t hash = 5381u;
    const char* end = data + len;

    while(data < end)
        hash = ((hash << 5) + hash) ^ static_cast<std::uint8_t>(*data++);

    return hash;
}

inline std::uint32_t Djb2(StringView str) noexcept
{
    return Djb2(str.Data(), str.Size());
}

inline constexpr std::uint32_t Adler32(const std::uint8_t* data, std::size_t len) noexcept
{
    constexpr std::uint32_t MOD = 65521u;
    std::uint32_t a = 1, b = 0;

    while(len > 0) {
        std::size_t chunk = len > 5552 ? 5552 : len;
        len -= chunk;

        while(chunk--) {
            a += *data++;
            b += a;
        }

        a %= MOD;
        b %= MOD;
    }

    return (b << 16) | a;
}

struct RollingHash {
    static constexpr std::uint64_t BASE = 131ull;
    static constexpr std::uint64_t MOD  = (1ull << 61) - 1; // Mersenne prime

    std::uint64_t hash   = 0;
    std::uint64_t power  = 1;
    std::uint32_t window = 0;

    inline void Push(std::uint8_t byte) noexcept
    {
        hash  = (hash * BASE + byte) % MOD;
        power = (power * BASE) % MOD;
        ++window;
    }

    inline void Roll(std::uint8_t outgoing, std::uint8_t incoming) noexcept
    {
        hash = (hash + MOD - (outgoing * power) % MOD) % MOD;
        hash = (hash * BASE + incoming) % MOD;
    }

    inline std::uint64_t Value() const noexcept { return hash; }
};

} // namespace Hasher

} // namespace WFX::Shared

#endif // WFX_SHARED_UTILS_HASH_HPP