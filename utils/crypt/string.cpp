#include "string.hpp"

#include "utils/backport/string.hpp"

namespace WFX::Utils {

// Char Utilities
std::uint8_t StringGuard::ToLowerAscii(std::uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? (c | 0x20) : c;
}

// String Utilties
bool StringGuard::CTStringCompare(std::string_view lhs, std::string_view rhs)
{
    if(lhs.size() != rhs.size()) return false;

    unsigned char result = 0;
    for(std::size_t i = 0; i < lhs.size(); ++i)
        result |= static_cast<unsigned char>(lhs[i]) ^ static_cast<unsigned char>(rhs[i]);

    return result == 0;
}

bool StringGuard::CTInsensitiveStringCompare(std::string_view lhs, std::string_view rhs)
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

bool StringGuard::CaseInsensitiveCompare(std::string_view lhs, std::string_view rhs)
{
    if(lhs.size() != rhs.size())
        return false;

    for(std::size_t i = 0; i < lhs.size(); ++i)
        if(ToLowerAscii(lhs[i]) != ToLowerAscii(rhs[i]))
            return false;

    return true;
}

// Path normalization
bool StringGuard::NormalizeURIPathInplace(std::string_view& path)
{
    // Sanity check
    if(path.size() == 0 || path.data() == nullptr)
        return false;

    char*       buf = const_cast<char*>(path.data());
    std::size_t len = path.size();

    char* read  = buf;
    char* write = buf;
    char* segments[256];
    int   segCount = 0;
    
    const char* end = buf + len;

    // Path must start with '/'
    if(*read != '/') return false;

    // Copy the first '/'
    *write++ = *read++;

    while(read < end) {
        // Collapse repeated slashes
        while(read < end && *read == '/') ++read;
        if(read >= end) break;

        char* segmentStart = write;

        // Copy segment or reject bad input
        while(read < end && *read != '/') {
            char c = *read;

            // Reject non-ASCII and control characters
            if((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F)
                return false;

            // Back slashes are not allowed
            if(c == '\\') return false;

            // Check for percent-encoded characters
            if(c == '%') {
                if(end - read < 3) return false;

                std::uint8_t h1 = UInt8FromHexChar(read[1]);
                std::uint8_t h2 = UInt8FromHexChar(read[2]);

                if(h1 == 0xFF || h2 == 0xFF) return false;  // Invalid hex

                std::uint8_t decoded = (h1 << 4) | h2;
                if(decoded <= 0x1F || decoded >= 0x7F)                  return false;  // Ctrl / Non-ASCII
                if(decoded == '/' || decoded == '\\' || decoded == '.') return false;
                if(decoded == '%')                                      return false;  // Prevent double-encoding like %252e
            }

            *write++ = *read++;
        }

        std::size_t segLen = write - segmentStart;

        if(segLen == 1 && segmentStart[0] == '.') {
            write = segmentStart; // Ignore current segment
            continue;
        }

        if(segLen == 2 && segmentStart[0] == '.' && segmentStart[1] == '.') {
            if(segCount == 0)
                return false;

            write = segments[--segCount];
            continue;
        }

        // Valid segment
        segments[segCount++] = segmentStart;
        *write++ = '/';
    }

    // Remove trailing slash unless root
    if(write > buf + 1 && *(write - 1) == '/')
        --write;

    *write = '\0';
    path = std::string_view(buf, write - buf);
    
    return true;
}

} // namespace WFX::Utils