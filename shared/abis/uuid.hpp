// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_ABI_UUID_HPP
#define WFX_SHARED_ABI_UUID_HPP

#include "string_view.hpp"
#include <cstring>

namespace WFX::Shared {

struct UUIDString {
    char data[37];
};

static_assert(sizeof(UUIDString) == 37, "'WFX_UUID_STRING' must be 37 bytes");
static_assert(std::is_standard_layout<UUIDString>::value, "'WFX_UUID_STRING' must be standard layout");
static_assert(std::is_trivially_copyable<UUIDString>::value, "'WFX_UUID_STRING' must be trivially copyable");

struct UUID {
public:
    std::uint8_t bytes[16];

public: // vvv Basic methods vvv
    void Clear() noexcept
    {
        std::memset(bytes, 0, 16);
    }

    bool IsZero() const noexcept
    {
        return std::memcmp(bytes, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0;
    }

public: // vvv Comparison vvv
    bool Equals(const UUID& other) const noexcept
    {
        return std::memcmp(bytes, other.bytes, 16) == 0;
    }
    bool operator==(const UUID& other) const noexcept
    {
        return Equals(other);
    }
    bool operator!=(const UUID& other) const noexcept
    {
        return !Equals(other);
    }

public: // vvv Factory vvv
    static UUID Zero() noexcept
    {
        UUID id{};
        id.Clear();
        return id;
    }

    static UUID FromBytes(const std::uint8_t* data) noexcept
    {
        UUID id{};
        if(data)
            std::memcpy(id.bytes, data, 16);
        else
            id.Clear();

        return id;
    }

    // clang-format off
    static bool FromString(StringView str, UUID& out) noexcept
    {
        if(str.Size() != 36)
            return false;

        const char* s = str.Data();

        // Validate dashes at fixed positions
        if(s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
            return false;

        // Dawg...
        static constexpr std::uint8_t kDecode[256] = {
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0x00
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0x10
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0x20
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0x30 '0'-'9'
            0xFF,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0x40 'A'-'F'
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0x50
            0xFF,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0x60 'a'-'f'
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0x70
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0x80
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0x90
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0xA0
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0xB0
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0xC0
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0xD0
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0xE0
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, // 0xF0
        };

        // UUID positions (skipping dashes at 8, 13, 18, 23):
        // 0-7, 9-12, 14-17, 19-22, 24-35
        #define P(dst, i) \
            { \
                const std::uint8_t hi = kDecode[(std::uint8_t)s[i]];       \
                const std::uint8_t lo = kDecode[(std::uint8_t)s[(i) + 1]]; \
                if(hi == 0xFF || lo == 0xFF) return false;                 \
                out.bytes[dst] = (hi << 4) | lo;                           \
            }

        P( 0,  0) P( 1,  2) P( 2,  4) P( 3,  6)                     // 8 chars
        P( 4,  9) P( 5, 11)                                         // 4 chars
        P( 6, 14) P( 7, 16)                                         // 4 chars
        P( 8, 19) P( 9, 21)                                         // 4 chars
        P(10, 24) P(11, 26) P(12, 28) P(13, 30) P(14, 32) P(15, 34) // 12 chars

        #undef P

        return true;
    }
    // clang-format on

    static bool FromString(const char* str, UUID& out) noexcept
    {
        if(!str)
            return false;

        // A UUID is exactly 36 chars. Probe the length with a 37-byte cap instead of assuming-
        // -36: the StringView overload reads s[0..35], so handing it a shorter string would read-
        // -past the buffer. strnlen stops at the NUL for any proper C-string (the contract here),-
        // -so a shorter/longer string is rejected without ever touching byte 36
        return ::strnlen(str, 37) == 36 && FromString(StringView{str, 36}, out);
    }

    // clang-format off
    UUIDString ToString() const noexcept
    {
        UUIDString out;
        char* b = out.data;

        // lmao wtf?
        static constexpr char kHex[513] = "000102030405060708090a0b0c0d0e0f"
                                          "101112131415161718191a1b1c1d1e1f"
                                          "202122232425262728292a2b2c2d2e2f"
                                          "303132333435363738393a3b3c3d3e3f"
                                          "404142434445464748494a4b4c4d4e4f"
                                          "505152535455565758595a5b5c5d5e5f"
                                          "606162636465666768696a6b6c6d6e6f"
                                          "707172737475767778797a7b7c7d7e7f"
                                          "808182838485868788898a8b8c8d8e8f"
                                          "909192939495969798999a9b9c9d9e9f"
                                          "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
                                          "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
                                          "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
                                          "d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
                                          "e0e1e2e3e4e5e6e7e8e9eaebecedeeef"
                                          "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

        // Each byte maps to 2 chars via kHex[byte*2]
        // UUID format: 8-4-4-4-12
        #define W(i) \
            b[0] = kHex[(std::uint8_t)bytes[i] * 2]; \
            b[1] = kHex[(std::uint8_t)bytes[i] * 2 + 1]; \
            b += 2

        #define D \
            *b++ = '-'

        W(0); W(1); W(2); W(3); D;
        W(4); W(5); D;
        W(6); W(7); D;
        W(8); W(9); D;
        W(10); W(11); W(12); W(13); W(14); W(15);

        #undef W
        #undef D

        out.data[36] = '\0';
        return out;
    }
    // clang-format on
};

static_assert(sizeof(UUID) == 16, "'WFX_UUID' must be 16 bytes");
static_assert(std::is_standard_layout<UUID>::value, "'WFX_UUID' must be standard layout");
static_assert(std::is_trivially_copyable<UUID>::value, "'WFX_UUID' must be trivially copyable");

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_UUID_HPP