#ifndef WFX_SHARED_ABI_UUID_HPP
#define WFX_SHARED_ABI_UUID_HPP

#include "string_view.hpp"
#include <cstring>

namespace WFX::Shared {

struct alignas(8) UUID {
public:
    std::uint8_t bytes[16];

public: // vvv Basic methods vvv
    void Clear() noexcept
    {
        std::memset(bytes, 0, 16);
    }

    bool IsZero() const noexcept
    {
        for(std::uint32_t i = 0; i < 16; ++i)
            if(bytes[i] != 0)
                return false;

        return true;
    }

public: // vvv Comparison vvv
    bool Equals(const UUID& other)     const noexcept { return std::memcmp(bytes, other.bytes, 16) == 0; }
    bool operator==(const UUID& other) const noexcept { return Equals(other); }
    bool operator!=(const UUID& other) const noexcept { return !Equals(other); }

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

    static bool FromString(StringView str, UUID& out) noexcept
    {
        if(str.Size() != 36)
            return false;

        const char* s = str.Data();

        // Validate dashes
        if(s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-')
            return false;

        auto hexToByte = [](std::uint8_t c) -> int {
            std::uint8_t lo = c - '0';
            std::uint8_t hi = (c | 0x20) - 'a';

            std::uint8_t isDigit = (lo <= 9);
            std::uint8_t isHex   = (hi <= 5);

            return isDigit * lo + isHex * (hi + 10) + (1 - (isDigit | isHex)) * 0xFF;
        };

        std::uint8_t result[16];
        std::uint32_t ri = 0;

        for(std::uint32_t i = 0; i < 36; ) {
            if(s[i] == '-') {
                ++i;
                continue;
            }

            int hi = hexToByte(s[i]);
            int lo = hexToByte(s[i + 1]);

            if(hi < 0 || lo < 0)
                return false;

            result[ri++] = static_cast<std::uint8_t>((hi << 4) | lo);
            i += 2;
        }

        if(ri != 16)
            return false;

        std::memcpy(out.bytes, result, 16);
        return true;
    }
};

static_assert(sizeof(UUID) == 16,                      "WFX_UUID must be 16 bytes");
static_assert(alignof(UUID) == 8,                      "WFX_UUID alignment mismatch");
static_assert(std::is_standard_layout<UUID>::value,    "WFX_UUID must be standard layout");
static_assert(std::is_trivially_copyable<UUID>::value, "WFX_UUID must be trivially copyable");

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_UUID_HPP