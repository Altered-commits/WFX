#ifndef WFX_UTILS_STRING_HPP
#define WFX_UTILS_STRING_HPP

#include <cstdint>
#include <string_view>

namespace WFX::Utils {

namespace StringCanonical {
    std::uint8_t ToLowerAscii(std::uint8_t c) noexcept;

    // Comparisions
    bool CTStringCompare(std::string_view lhs, std::string_view rhs)            noexcept;
    bool CTInsensitiveStringCompare(std::string_view lhs, std::string_view rhs) noexcept;
    bool InsensitiveStringCompare(std::string_view lhs, std::string_view rhs)   noexcept;

    // Normalizations
    /* NOTE: 'path' must be a valid writable buffer */
    bool        NormalizeURIPathInplace(std::string_view& path)                           noexcept;
    std::string NormalizePathToIdentifier(std::string_view path, std::string_view prefix) noexcept;
    /* NOTE: 'buf' must be a valid writable buffer */
    bool        DecodePercentInplace(std::string_view& buf)                               noexcept;
} // namespace StringCanonical

} // namespace WFX::Utils


#endif // WFX_UTILS_STRING_HPP