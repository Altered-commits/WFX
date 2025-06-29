#include "string.hpp"

namespace WFX::Utils {

// Char Utilities
std::uint8_t ToLowerAscii(std::uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? (c | 0x20) : c;
}

// String Utilties
bool CTStringCompare(std::string_view lhs, std::string_view rhs)
{
    if(lhs.size() != rhs.size()) return false;

    unsigned char result = 0;
    for(std::size_t i = 0; i < lhs.size(); ++i)
        result |= static_cast<unsigned char>(lhs[i]) ^ static_cast<unsigned char>(rhs[i]);

    return result == 0;
}

bool CTInsensitiveStringCompare(std::string_view lhs, std::string_view rhs)
{
    if(lhs.size() != rhs.size()) return false;

    unsigned char result = 0;
    for(std::size_t i = 0; i < lhs.size(); ++i)
    {
        // Normalize both chars to lowercase before XOR
        unsigned char l = static_cast<unsigned char>(ToLowerAscii(static_cast<unsigned char>(lhs[i])));
        unsigned char r = static_cast<unsigned char>(ToLowerAscii(static_cast<unsigned char>(rhs[i])));
        result |= l ^ r;
    }

    return result == 0;
}

} // namespace WFX::Utils