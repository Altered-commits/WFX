#ifndef WFX_UTILS_STRING_UTILS_HPP
#define WFX_UTILS_STRING_UTILS_HPP

#include "utils/crypt/string.hpp"

#include <string>
#include <charconv>

// Bunch of functions which exist in C++20 that don't exist in C++17
// or
// Just String utility functions
namespace WFX::Utils {

// vvv Comparisions vvv
inline constexpr bool StartsWith(std::string_view str, std::string_view prefix) noexcept
{
    return str.size() >= prefix.size() &&
           str.compare(0, prefix.size(), prefix) == 0;
}

inline constexpr bool EndsWith(std::string_view str, std::string_view suffix) noexcept
{
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// vvv Conversions vvv
inline std::string UInt64ToStr(uint64_t value, const std::string& fallback = "0") noexcept
{
    // Max decimal digits for uint64_t = 20, plus one for null terminator
    char buf[21];
    
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    
    if(ec != std::errc())
        return fallback;

    return std::string(buf, ptr);
}

inline constexpr bool StrToUInt64(std::string_view str, std::uint64_t& out) noexcept
{
    if(str.empty())
        return false;

    std::uint64_t result = 0;
    constexpr std::uint64_t kMaxDiv10 = UINT64_MAX / 10;
    constexpr std::uint64_t kMaxMod10 = UINT64_MAX % 10;

    for(char c : str) {
        if(c < '0' || c > '9')
            return false;

        std::uint64_t digit = static_cast<std::uint64_t>(c - '0');

        if(result > kMaxDiv10 || (result == kMaxDiv10 && digit > kMaxMod10))
            return false;

        result = result * 10 + digit;
    }

    out = result;
    return true;
}

inline constexpr bool StrToInt64(std::string_view str, std::int64_t& out) noexcept
{
    if(str.empty())
        return false;

    std::size_t i = 0;
    bool negative = (str[0] == '-');
    
    if(negative) {
        if(str.size() == 1) return false; // Just "-"
        i = 1;
    }

    constexpr std::uint64_t kMaxDiv10 = static_cast<std::uint64_t>(INT64_MAX) / 10;
    constexpr std::uint64_t kMaxMod10 = static_cast<std::uint64_t>(INT64_MAX) % 10;

    std::uint64_t result = 0;
    for(; i < str.size(); ++i) {
        char c = str[i];
        if (c < '0' || c > '9')
            return false;

        std::uint64_t digit = static_cast<std::uint64_t>(c - '0');

        if(result > kMaxDiv10 || (result == kMaxDiv10 && digit > (kMaxMod10 + static_cast<std::uint64_t>(negative))))
            return false;

        result = result * 10 + digit;
    }

    out = negative ? -static_cast<std::int64_t>(result) : static_cast<std::int64_t>(result);

    return true;
}

inline constexpr std::uint8_t UInt8FromHexChar(std::uint8_t uc) noexcept
{
    // Convert ASCII '0'-'9', 'a'-'f', 'A'-'F' to 0-15. Invalid chars return 0xFF
    uint8_t lo = uc - '0';
    uint8_t hi = (uc | 0x20) - 'a';  // case-insensitive: 'A'-'F' and 'a'-'f'

    uint8_t isDigit = (lo < 10);
    uint8_t isHex   = (hi < 6);

    return (isDigit * lo) | (isHex * (hi + 10)) | ((isDigit | isHex) ? 0 : 0xFF);
}

// vvv Misc vvv
inline void TrimInline(std::string& s) noexcept
{
    std::size_t start = 0;
    std::size_t end   = s.size();

    // Trim front
    while(start < end && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;

    // Trim back
    while(end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;

    // Shift characters in-place
    if(start > 0) {
        std::size_t i = 0;
        for(; start < end; ++i, ++start)
            s[i] = s[start];
    
        s.resize(i);
    }
    // Just resize to trim end
    else
        s.resize(end);
}

inline std::string_view TrimView(std::string_view sv) noexcept
{
    std::size_t start = 0;
    std::size_t end   = sv.size();

    // Trim front
    while(start < end && std::isspace(static_cast<unsigned char>(sv[start])))
        ++start;

    // Trim back
    while(end > start && std::isspace(static_cast<unsigned char>(sv[end - 1])))
        --end;

    return sv.substr(start, end - start);
}

} // namespace WFX::Utils


#endif // WFX_UTILS_STRING_UTILS_HPP