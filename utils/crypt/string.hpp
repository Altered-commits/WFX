#ifndef WFX_UTILS_STRING_HPP
#define WFX_UTILS_STRING_HPP

#include <cstdint>
#include <string_view>

namespace WFX::Utils {

class StringGuard final {
public:
    static std::uint8_t ToLowerAscii(std::uint8_t c);

    // Constant time comparisions
    static bool CTStringCompare(std::string_view lhs, std::string_view rhs);
    static bool CTInsensitiveStringCompare(std::string_view lhs, std::string_view rhs);

    static bool CaseInsensitiveCompare(std::string_view lhs, std::string_view rhs);

    /* NOTE: 'path' buffer must be a valid writable buffer */
    static bool NormalizeURIPathInplace(std::string_view& path);

private:
    StringGuard()  = delete;
    ~StringGuard() = delete;
};

} // namespace WFX::Utils


#endif // WFX_UTILS_STRING_HPP