#ifndef WFX_UTILS_STRING_HPP
#define WFX_UTILS_STRING_HPP

#include <string_view>

namespace WFX::Utils {

// Some ASCII utilities
std::uint8_t ToLowerAscii(std::uint8_t c);

// Constant time comparisions
bool CTStringCompare(std::string_view lhs, std::string_view rhs);
bool CTInsensitiveStringCompare(std::string_view lhs, std::string_view rhs);

} // namespace WFX::Utils


#endif // WFX_UTILS_STRING_HPP