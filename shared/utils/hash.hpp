// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_UTILS_HASH_HPP
#define WFX_SHARED_UTILS_HASH_HPP

#include "detection_macro.hpp"
#include "shared/abis/string_view.hpp"
#include <bit>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace WFX::Shared {

namespace Hasher {

// vvv Primitives shared by every hash below vvv
namespace Detail {

// memcpy avoids strict-aliasing/alignment UB on an unaligned read
inline std::uint32_t Read32(const std::uint8_t* p) noexcept
{
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline std::uint64_t Read64(const std::uint8_t* p) noexcept
{
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

// Every mainstream compiler (GCC, Clang, MSVC, ICX) pattern-matches this exact shift-mask-
// -sequence into a single native bswap instruction, so this costs nothing over a compiler-
// -specific builtin while staying portable to any standards-conforming compiler
inline constexpr std::uint32_t ByteSwap32(std::uint32_t x) noexcept
{
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) | ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}

inline constexpr std::uint64_t ByteSwap64(std::uint64_t x) noexcept
{
    return ((x & 0x00000000000000FFull) << 56) | ((x & 0x000000000000FF00ull) << 40) |
           ((x & 0x0000000000FF0000ull) << 24) | ((x & 0x00000000FF000000ull) << 8) |
           ((x & 0x000000FF00000000ull) >> 8) | ((x & 0x0000FF0000000000ull) >> 24) |
           ((x & 0x00FF000000000000ull) >> 40) | ((x & 0xFF00000000000000ull) >> 56);
}

// 64x64->128 multiply, folded to 64 bits (low ^ high)
inline std::uint64_t Mix(std::uint64_t a, std::uint64_t b) noexcept
{
#if defined(WFX_COMPILER_MSVC)
    std::uint64_t hi;
    std::uint64_t lo = _umul128(a, b, &hi);
    return lo ^ hi;
#elif defined(__SIZEOF_INT128__)
    const __uint128_t r = static_cast<__uint128_t>(a) * b;
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

} // namespace Detail

namespace WyHashDetail {

static constexpr std::uint64_t SECRET0 = 0xa0761d6478bd642full;
static constexpr std::uint64_t SECRET1 = 0xe7037ed1a0b428dbull;
static constexpr std::uint64_t SECRET2 = 0x8ebc6af09c88c6e3ull;
static constexpr std::uint64_t SECRET3 = 0x589965cc75374cc3ull;

} // namespace WyHashDetail

inline std::uint64_t WyHash(const char* key, std::size_t len, std::uint64_t seed = 0) noexcept
{
    using namespace Detail;
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
            a = (static_cast<std::uint64_t>(Read32(p)) << 32) | Read32(p + ((len >> 3) << 2));
            b = (static_cast<std::uint64_t>(Read32(p + len - 4)) << 32) | Read32(p + len - 4 - ((len >> 3) << 2));
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
                seed = Mix(Read64(p) ^ SECRET1, Read64(p + 8) ^ seed);
                s = Mix(Read64(p + 16) ^ SECRET2, Read64(p + 24) ^ s);
                see2 = Mix(Read64(p + 32) ^ SECRET3, Read64(p + 40) ^ see2);
                p += 48;
                i -= 48;
            } while(i > 48);

            seed ^= s ^ see2;
        }

        while(i > 16) [[unlikely]] {
            seed = Mix(Read64(p) ^ SECRET1, Read64(p + 8) ^ seed);
            p += 16;
            i -= 16;
        }

        // Final two overlapping 8-byte reads consume the tail
        a = Read64(p + i - 16);
        b = Read64(p + i - 8);
    }

    return Mix(SECRET1 ^ static_cast<std::uint64_t>(len), Mix(a ^ SECRET1, b ^ seed));
}

inline std::uint64_t WyHash(StringView str, std::uint64_t seed = 0) noexcept
{
    return WyHash(str.Data(), str.Size(), seed);
}

inline constexpr std::uint64_t Fnv1a(const char* data, std::uint64_t len) noexcept
{
    constexpr std::uint64_t PRIME = 1099511628211ULL;
    constexpr std::uint64_t OFFSET = 14695981039346656037ULL;

    std::uint64_t hash = OFFSET;
    const char* end = data + len;

    while(data < end) {
        hash ^= *data++;
        hash *= PRIME;
    }

    return hash;
}

inline std::uint64_t Fnv1a(StringView str) noexcept
{
    return Fnv1a(str.Data(), static_cast<std::uint64_t>(str.Size()));
}

inline constexpr std::uint64_t Fnv1aCaseInsensitive(const char* data, std::uint64_t len) noexcept
{
    constexpr std::uint64_t PRIME = 1099511628211ULL;
    constexpr std::uint64_t OFFSET = 14695981039346656037ULL;

    std::uint64_t hash = OFFSET;
    const char* end = data + len;

    while(data < end) {
        const std::uint8_t c = *data++;
        hash ^= static_cast<std::uint8_t>((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
        hash *= PRIME;
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

namespace Xxh3Detail {

using Detail::ByteSwap32;
using Detail::ByteSwap64;
using Detail::Mix;
using Detail::Read32;
using Detail::Read64;

// The reference implementation's default 192-byte secret, byte-identical to xxHash's kSecret
static constexpr std::uint8_t SECRET[192] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c, 0xde, 0xd4,
    0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f, 0xcb, 0x79, 0xe6, 0x4e,
    0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21, 0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43,
    0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c, 0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb,
    0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3, 0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19,
    0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8, 0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7,
    0xc7, 0x0b, 0x4f, 0x1d, 0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78,
    0x73, 0x64, 0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e, 0x2b, 0x16,
    0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce, 0x45, 0xcb, 0x3a, 0x8f,
    0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};

static constexpr std::uint32_t PRIME32_1 = 0x9E3779B1U;
static constexpr std::uint32_t PRIME32_2 = 0x85EBCA77U;
static constexpr std::uint32_t PRIME32_3 = 0xC2B2AE3DU;
static constexpr std::uint64_t PRIME64_1 = 0x9E3779B185EBCA87ULL;
static constexpr std::uint64_t PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;
static constexpr std::uint64_t PRIME64_3 = 0x165667B19E3779F9ULL;
static constexpr std::uint64_t PRIME64_4 = 0x85EBCA77C2B2AE63ULL;
static constexpr std::uint64_t PRIME64_5 = 0x27D4EB2F165667C5ULL;
static constexpr std::uint64_t PRIME_MX1 = 0x165667919E3779F9ULL;
static constexpr std::uint64_t PRIME_MX2 = 0x9FB21C651E98DF25ULL;

static constexpr int STRIPE_LEN = 64;
static constexpr int ACC_NB = 8;
static constexpr int SECRET_SIZE = 192;
static constexpr int SECRET_CONSUME_RATE = 8;
static constexpr int NB_ROUNDS = (SECRET_SIZE - STRIPE_LEN) / SECRET_CONSUME_RATE;

inline std::uint64_t Xxh64Avalanche(std::uint64_t h) noexcept
{
    h ^= h >> 33;
    h *= PRIME64_2;
    h ^= h >> 29;
    h *= PRIME64_3;
    h ^= h >> 32;
    return h;
}

inline std::uint64_t Avalanche(std::uint64_t h) noexcept
{
    h ^= h >> 37;
    h *= PRIME_MX1;
    h ^= h >> 32;
    return h;
}

inline std::uint64_t Rrmxmx(std::uint64_t h, std::uint64_t len) noexcept
{
    h ^= std::rotl(h, 49) ^ std::rotl(h, 24);
    h *= PRIME_MX2;
    h ^= (h >> 35) + len;
    h *= PRIME_MX2;
    h ^= h >> 28;
    return h;
}

inline std::uint64_t Len0(std::uint64_t seed) noexcept
{
    return Xxh64Avalanche(seed ^ (Read64(SECRET + 56) ^ Read64(SECRET + 64)));
}

inline std::uint64_t Len1to3(const std::uint8_t* in, std::size_t len, std::uint64_t seed) noexcept
{
    const std::uint8_t c1 = in[0], c2 = in[len >> 1], c3 = in[len - 1];
    const auto combined = static_cast<std::uint32_t>((c1 << 16) | (c2 << 24) | c3 | (len << 8));
    const std::uint64_t bitflip = static_cast<std::uint64_t>(Read32(SECRET) ^ Read32(SECRET + 4)) + seed;
    return Xxh64Avalanche(static_cast<std::uint64_t>(combined) ^ bitflip);
}

inline std::uint64_t Len4to8(const std::uint8_t* in, std::size_t len, std::uint64_t seed) noexcept
{
    seed ^= static_cast<std::uint64_t>(ByteSwap32(static_cast<std::uint32_t>(seed))) << 32;
    const std::uint64_t bitflip = (Read64(SECRET + 8) ^ Read64(SECRET + 16)) - seed;
    const std::uint32_t input1 = Read32(in);
    const std::uint32_t input2 = Read32(in + len - 4);
    const std::uint64_t input64 = input2 + (static_cast<std::uint64_t>(input1) << 32);
    return Rrmxmx(input64 ^ bitflip, len);
}

inline std::uint64_t Len9to16(const std::uint8_t* in, std::size_t len, std::uint64_t seed) noexcept
{
    const std::uint64_t bitflip1 = (Read64(SECRET + 24) ^ Read64(SECRET + 32)) + seed;
    const std::uint64_t bitflip2 = (Read64(SECRET + 40) ^ Read64(SECRET + 48)) - seed;
    const std::uint64_t lo = Read64(in) ^ bitflip1;
    const std::uint64_t hi = Read64(in + len - 8) ^ bitflip2;
    const std::uint64_t acc = len + ByteSwap64(lo) + hi + Mix(lo, hi);
    return Avalanche(acc);
}

inline std::uint64_t Len0to16(const std::uint8_t* in, std::size_t len, std::uint64_t seed) noexcept
{
    if(len > 8)
        return Len9to16(in, len, seed);
    if(len >= 4)
        return Len4to8(in, len, seed);
    if(len)
        return Len1to3(in, len, seed);

    return Len0(seed);
}

inline std::uint64_t Mix16B(const std::uint8_t* in, const std::uint8_t* secret, std::uint64_t seed) noexcept
{
    const std::uint64_t lo = Read64(in), hi = Read64(in + 8);
    return Mix(lo ^ (Read64(secret) + seed), hi ^ (Read64(secret + 8) - seed));
}

inline std::uint64_t Len17to128(const std::uint8_t* in, std::size_t len, std::uint64_t seed) noexcept
{
    std::uint64_t acc = len * PRIME64_1;

    if(len > 32) {
        if(len > 64) {
            if(len > 96) {
                acc += Mix16B(in + 48, SECRET + 96, seed);
                acc += Mix16B(in + len - 64, SECRET + 112, seed);
            }
            acc += Mix16B(in + 32, SECRET + 64, seed);
            acc += Mix16B(in + len - 48, SECRET + 80, seed);
        }
        acc += Mix16B(in + 16, SECRET + 32, seed);
        acc += Mix16B(in + len - 32, SECRET + 48, seed);
    }
    acc += Mix16B(in, SECRET, seed);
    acc += Mix16B(in + len - 16, SECRET + 16, seed);

    return Avalanche(acc);
}

inline std::uint64_t Len129to240(const std::uint8_t* in, std::size_t len, std::uint64_t seed) noexcept
{
    std::uint64_t acc = len * PRIME64_1;
    const int rounds = static_cast<int>(len / 16);

    for(int i = 0; i < 8; ++i)
        acc += Mix16B(in + 16 * i, SECRET + 16 * i, seed);

    acc = Avalanche(acc);

    for(int i = 8; i < rounds; ++i)
        acc += Mix16B(in + 16 * i, SECRET + 16 * (i - 8) + 3, seed);

    acc += Mix16B(in + len - 16, SECRET + 136 - 17, seed);
    return Avalanche(acc);
}

inline void Accumulate512(std::uint64_t* acc, const std::uint8_t* in, const std::uint8_t* secret) noexcept
{
    for(int i = 0; i < ACC_NB; ++i) {
        const std::uint64_t dataVal = Read64(in + 8 * i);
        const std::uint64_t dataKey = dataVal ^ Read64(secret + 8 * i);
        acc[i ^ 1] += dataVal;
        acc[i] +=
            static_cast<std::uint32_t>(dataKey) * static_cast<std::uint64_t>(static_cast<std::uint32_t>(dataKey >> 32));
    }
}

inline void ScrambleAcc(std::uint64_t* acc, const std::uint8_t* secret) noexcept
{
    for(int i = 0; i < ACC_NB; ++i) {
        acc[i] ^= acc[i] >> 47;
        acc[i] ^= Read64(secret + 8 * i);
        acc[i] *= PRIME32_1;
    }
}

inline void Accumulate(std::uint64_t* acc, const std::uint8_t* in, const std::uint8_t* secret,
                       std::size_t nbStripes) noexcept
{
    for(std::size_t n = 0; n < nbStripes; ++n)
        Accumulate512(acc, in + n * STRIPE_LEN, secret + n * SECRET_CONSUME_RATE);
}

inline std::uint64_t Mix2Accs(const std::uint64_t* acc, const std::uint8_t* secret) noexcept
{
    return Mix(acc[0] ^ Read64(secret), acc[1] ^ Read64(secret + 8));
}

inline std::uint64_t MergeAccs(const std::uint64_t* acc, const std::uint8_t* secret, std::uint64_t start) noexcept
{
    std::uint64_t result = start;
    for(int i = 0; i < 4; ++i)
        result += Mix2Accs(acc + 2 * i, secret + 16 * i);

    return Avalanche(result);
}

// Derives a per-seed secret for the long-input path (real XXH3 never reuses the raw default-
// -secret once seed != 0); each 16-byte pair of the default secret gets +seed/-seed applied
inline void InitCustomSecret(std::uint8_t* out, std::uint64_t seed) noexcept
{
    for(int i = 0; i < SECRET_SIZE / 16; ++i) {
        const std::uint64_t lo = Read64(SECRET + i * 16) + seed;
        const std::uint64_t hi = Read64(SECRET + i * 16 + 8) - seed;
        std::memcpy(out + i * 16, &lo, 8);
        std::memcpy(out + i * 16 + 8, &hi, 8);
    }
}

inline std::uint64_t HashLong(const std::uint8_t* in, std::size_t len, std::uint64_t seed) noexcept
{
    std::uint8_t customSecret[SECRET_SIZE];
    const std::uint8_t* secret = SECRET;
    if(seed != 0) {
        InitCustomSecret(customSecret, seed);
        secret = customSecret;
    }

    std::uint64_t acc[ACC_NB] = {PRIME32_3, PRIME64_1, PRIME64_2, PRIME64_3,
                                 PRIME64_4, PRIME32_2, PRIME64_5, PRIME32_1};

    const std::size_t nbBlocks = (len - 1) / (STRIPE_LEN * NB_ROUNDS);
    for(std::size_t n = 0; n < nbBlocks; ++n) {
        Accumulate(acc, in + n * STRIPE_LEN * NB_ROUNDS, secret, NB_ROUNDS);
        ScrambleAcc(acc, secret + SECRET_SIZE - STRIPE_LEN);
    }

    const std::size_t nbStripes = ((len - 1) - (STRIPE_LEN * NB_ROUNDS * nbBlocks)) / STRIPE_LEN;
    Accumulate(acc, in + nbBlocks * STRIPE_LEN * NB_ROUNDS, secret, nbStripes);

    Accumulate512(acc, in + len - STRIPE_LEN, secret + SECRET_SIZE - STRIPE_LEN - 7);

    return MergeAccs(acc, secret + 11, static_cast<std::uint64_t>(len) * PRIME64_1);
}

} // namespace Xxh3Detail

inline std::uint64_t Xxh3(const char* data, std::size_t len, std::uint64_t seed = 0) noexcept
{
    using namespace Xxh3Detail;
    const auto* in = reinterpret_cast<const std::uint8_t*>(data);

    if(len <= 16)
        return Len0to16(in, len, seed);
    if(len <= 128)
        return Len17to128(in, len, seed);
    if(len <= 240)
        return Len129to240(in, len, seed);

    return HashLong(in, len, seed);
}

inline std::uint64_t Xxh3(StringView str, std::uint64_t seed = 0) noexcept
{
    return Xxh3(str.Data(), str.Size(), seed);
}

} // namespace Hasher

} // namespace WFX::Shared

#endif // WFX_SHARED_UTILS_HASH_HPP
