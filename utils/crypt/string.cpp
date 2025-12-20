#include "string.hpp"

#include "utils/backport/string.hpp"
#include <algorithm>

namespace WFX::Utils {

// Char Utilities
std::uint8_t StringCanonical::ToLowerAscii(std::uint8_t c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? (c | 0x20) : c;
}

// String Utilties
bool StringCanonical::CTStringCompare(std::string_view lhs, std::string_view rhs) noexcept
{
    if(lhs.size() != rhs.size()) return false;

    unsigned char result = 0;
    for(std::size_t i = 0; i < lhs.size(); ++i)
        result |= static_cast<unsigned char>(lhs[i]) ^ static_cast<unsigned char>(rhs[i]);

    return result == 0;
}

bool StringCanonical::CTInsensitiveStringCompare(std::string_view lhs, std::string_view rhs) noexcept
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

bool StringCanonical::InsensitiveStringCompare(std::string_view lhs, std::string_view rhs) noexcept
{
    if(lhs.size() != rhs.size())
        return false;

    for(std::size_t i = 0; i < lhs.size(); ++i)
        if(ToLowerAscii(lhs[i]) != ToLowerAscii(rhs[i]))
            return false;

    return true;
}

// Path normalization
bool StringCanonical::NormalizeURIPathInplace(std::string_view& path) noexcept
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

std::string StringCanonical::NormalizePathToIdentifier(std::string_view path, std::string_view prefix) noexcept
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    
    out.reserve(prefix.size() + path.size() * 4);
    out += prefix;

    for(unsigned char c : path) {
        if(std::isalnum(c))
            out += c;
        else {
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// Percent normalization
bool StringCanonical::DecodePercentInplace(std::string_view& buf_) noexcept
{
    if(buf_.empty())
        return true;

    char*       buf = const_cast<char*>(buf_.data());
    std::size_t len = buf_.size();
    std::size_t idx = 0;

    for(std::size_t r = 0; r < len; ++r) {
        char c = buf[r];

        if(c == '+')
            buf[idx++] = ' ';
        
        else if(c == '%') {
            if(r + 2 >= len)
                return false;

            std::uint8_t hi = UInt8FromHexChar(static_cast<std::uint8_t>(buf[r + 1]));
            std::uint8_t lo = UInt8FromHexChar(static_cast<std::uint8_t>(buf[r + 2]));

            if((hi | lo) == 0xFF)
                return false;

            buf[idx++] = static_cast<char>((hi << 4) | lo);
            r += 2;
        }
        else
            buf[idx++] = c;
    }

    buf_ = std::string_view(buf, idx);
    return true;
}

} // namespace WFX::Utils