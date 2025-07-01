#ifndef WFX_UTILS_STRING_UTILS_HPP
#define WFX_UTILS_STRING_UTILS_HPP

#include "utils/crypt/string.hpp"

#include <string>
#include <charconv>

// Bunch of functions which exist in C++20 that don't exist in C++20
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

inline constexpr bool CaseInsensitiveCompare(std::string_view lhs, std::string_view rhs)
{
    if(lhs.size() != rhs.size())
        return false;

    for(std::size_t i = 0; i < lhs.size(); ++i)
        if(ToLowerAscii(lhs[i]) != ToLowerAscii(rhs[i]))
            return false;

    return true;
}

// vvv Conversions vvv
inline std::string UInt64ToStr(uint64_t value, const std::string& fallback = "0")
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

} // namespace WFX::Utils


#endif // WFX_UTILS_STRING_UTILS_HPP