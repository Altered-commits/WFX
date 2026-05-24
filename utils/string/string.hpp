#ifndef WFX_UTILS_STRING_HPP
#define WFX_UTILS_STRING_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace WFX::Utils {
namespace StringUtils {

// Comparisons
bool CTStringCompare(std::string_view lhs, std::string_view rhs)            noexcept;
bool CTInsensitiveStringCompare(std::string_view lhs, std::string_view rhs) noexcept;
bool InsensitiveStringCompare(std::string_view lhs, std::string_view rhs)   noexcept;

// Normalizations
// NOTE: 'path' must point into a mutable buffer owned by the caller
bool        NormalizeURIPathInplace(std::string_view& path)                           noexcept;
std::string NormalizePathToIdentifier(std::string_view path, std::string_view prefix) noexcept;
bool        DecodePercentInplace(std::string_view& buf)                               noexcept;

// Conversions
std::uint8_t ToLowerAscii(std::uint8_t c)                                          noexcept;
std::string  UInt64ToStr(std::uint64_t value, const std::string& fallback = "NaN") noexcept;
bool         StrToUInt64(std::string_view str, std::uint64_t& out)                 noexcept;
bool         StrToInt64(std::string_view str, std::int64_t& out)                   noexcept;

// Trim
void             TrimInline(std::string& s)    noexcept;
std::string_view TrimView(std::string_view sv) noexcept;

// vvv constexpr functions vvv
inline constexpr std::uint8_t UInt8FromHexChar(std::uint8_t uc) noexcept
{
    std::uint8_t lo = uc - '0';
    std::uint8_t hi = (uc | 0x20) - 'a';

    std::uint8_t isDigit = (lo < 10);
    std::uint8_t isHex   = (hi < 6);

    return (isDigit * lo) | (isHex * (hi + 10)) | ((isDigit | isHex) ? 0 : 0xFF);
}

} // namespace StringUtils
} // namespace WFX::Utils

#endif // WFX_UTILS_STRING_HPP