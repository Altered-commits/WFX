// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_UTILS_HASH_HPP
#define WFX_SHARED_UTILS_HASH_HPP

#include "detection_macro.hpp"
#include "shared/abis/string_view.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace WFX::Shared {

namespace Hasher {

namespace WyHashDetail {

static constexpr std::uint64_t SECRET0 = 0xa0761d6478bd642full;
static constexpr std::uint64_t SECRET1 = 0xe7037ed1a0b428dbull;
static constexpr std::uint64_t SECRET2 = 0x8ebc6af09c88c6e3ull;
static constexpr std::uint64_t SECRET3 = 0x589965cc75374cc3ull;

inline std::uint64_t Mix(std::uint64_t a, std::uint64_t b) noexcept
{
#if defined(WFX_COMPILER_MSVC)
    std::uint64_t hi;
    std::uint64_t lo = _umul128(a, b, &hi);
    return lo ^ hi;
#elif defined(__SIZEOF_INT128__)
    __uint128_t r = static_cast<__uint128_t>(a) * b;
    return static_cast<std::uint64_t>(r) ^ static_cast<std::uint64_t>(r >> 64);
#else
    // Split into 32-bit halves and reconstruct the full 128-bit product manually
    // Produces identical output to the __uint128_t path
    std::uint64_t a_lo = static_cast<std::uint32_t>(a), a_hi = a >> 32;
    std::uint64_t b_lo = static_cast<std::uint32_t>(b), b_hi = b >> 32;
    std::uint64_t ll = a_lo * b_lo;
    std::uint64_t lh = a_lo * b_hi;
    std::uint64_t hl = a_hi * b_lo;
    std::uint64_t hh = a_hi * b_hi;
    std::uint64_t mid = (ll >> 32) + static_cast<std::uint32_t>(lh) + static_cast<std::uint32_t>(hl);
    std::uint64_t hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    std::uint64_t lo = (mid << 32) | static_cast<std::uint32_t>(ll);
    return lo ^ hi;
#endif
}

inline std::uint64_t R64(const std::uint8_t* p) noexcept
{
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

inline std::uint64_t R32(const std::uint8_t* p) noexcept
{
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

} // namespace WyHashDetail

inline std::uint64_t WyHash(const char* key, std::size_t len, std::uint64_t seed = 0) noexcept
{
    using namespace WyHashDetail;

    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(key);
    std::uint64_t a{0}, b{0};
    seed ^= SECRET0;

    // Short path (<= 16 bytes)
    if(len <= 16) [[likely]] {
        if(len >= 4) [[likely]] {
            // Two overlapping 4-byte reads from head and tail, merged into 64 bits each
            // The (len >> 3) << 2 term picks the mid-point for 4..7 byte inputs so the-
            // -reads don't go out of bounds
            a = (static_cast<std::uint64_t>(R32(p)) << 32) | R32(p + ((len >> 3) << 2));
            b = (static_cast<std::uint64_t>(R32(p + len - 4)) << 32) | R32(p + len - 4 - ((len >> 3) << 2));
        }
        else if(len > 0) [[likely]] {
            // 1..3 bytes: pack first, middle, last into a single word
            a = (static_cast<std::uint64_t>(p[0]) << 16) | (static_cast<std::uint64_t>(p[len >> 1]) << 8) |
                static_cast<std::uint64_t>(p[len - 1]);
            b = 0;
        }
        else
            a = b = 0;
    }
    // Long path (> 16 bytes)
    else {
        std::size_t i = len;

        if(i > 48) [[unlikely]] {
            // Three independent accumulators keep the pipeline busy across-
            // -iterations, hiding multiply latency on out-of-order CPUs
            std::uint64_t s = seed, see2 = seed;

            do {
                seed = Mix(R64(p) ^ SECRET1, R64(p + 8) ^ seed);
                s = Mix(R64(p + 16) ^ SECRET2, R64(p + 24) ^ s);
                see2 = Mix(R64(p + 32) ^ SECRET3, R64(p + 40) ^ see2);
                p += 48;
                i -= 48;
            } while(i > 48);

            seed ^= s ^ see2;
        }

        while(i > 16) [[unlikely]] {
            seed = Mix(R64(p) ^ SECRET1, R64(p + 8) ^ seed);
            p += 16;
            i -= 16;
        }

        // Final two overlapping 8-byte reads consume the tail
        a = R64(p + i - 16);
        b = R64(p + i - 8);
    }

    return Mix(SECRET1 ^ static_cast<std::uint64_t>(len), Mix(a ^ SECRET1, b ^ seed));
}

inline std::uint64_t WyHash(StringView str, std::uint64_t seed = 0) noexcept
{
    return WyHash(str.Data(), str.Size(), seed);
}

inline constexpr std::uint64_t Fnv1a(const char* data, std::uint64_t len) noexcept
{
    constexpr std::uint64_t prime = 1099511628211ULL;
    constexpr std::uint64_t offset = 14695981039346656037ULL;

    std::uint64_t hash = offset;
    const char* end = data + len;

    while(data < end) {
        hash ^= *data++;
        hash *= prime;
    }

    return hash;
}

inline std::uint64_t Fnv1a(StringView str) noexcept
{
    return Fnv1a(str.Data(), static_cast<std::uint64_t>(str.Size()));
}

inline constexpr std::uint64_t Fnv1aCaseInsensitive(const char* data, std::uint64_t len) noexcept
{
    constexpr std::uint64_t prime = 1099511628211ULL;
    constexpr std::uint64_t offset = 14695981039346656037ULL;

    std::uint64_t hash = offset;
    const char* end = data + len;

    while(data < end) {
        std::uint8_t c = *data++;
        hash ^= static_cast<std::uint8_t>((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
        hash *= prime;
    }

    return hash;
}

inline std::uint64_t Fnv1aCaseInsensitive(StringView str) noexcept
{
    return Fnv1aCaseInsensitive(str.Data(), static_cast<std::uint64_t>(str.Size()));
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
template <typename T> inline std::uint64_t HashValue(const T& val) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>, "HashValue requires trivially copyable type");

    return Fnv1a(reinterpret_cast<const char*>(&val), sizeof(T));
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
    static constexpr std::uint64_t MOD = (1ull << 61) - 1; // Mersenne prime

    std::uint64_t hash = 0;
    std::uint64_t power = 1;
    std::uint32_t window = 0;

    inline void Push(std::uint8_t byte) noexcept
    {
        hash = (hash * BASE + byte) % MOD;
        power = (power * BASE) % MOD;
        ++window;
    }

    inline void Roll(std::uint8_t outgoing, std::uint8_t incoming) noexcept
    {
        hash = (hash + MOD - (outgoing * power) % MOD) % MOD;
        hash = (hash * BASE + incoming) % MOD;
    }

    inline std::uint64_t Value() const noexcept
    {
        return hash;
    }
};

} // namespace Hasher

} // namespace WFX::Shared

#endif // WFX_SHARED_UTILS_HASH_HPP