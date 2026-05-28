#include "utils/string/string.hpp"
#include <charconv>
#include <cctype>
#include <cstring>

namespace WFX::Utils {
namespace StringUtils {

// vvv Comparisons vvv
bool CTStringCompare(std::string_view lhs, std::string_view rhs) noexcept
{
    if(lhs.size() != rhs.size())
        return false;

    std::uint8_t result = 0;
    for(std::size_t i = 0; i < lhs.size(); ++i)
        result |= static_cast<std::uint8_t>(lhs[i]) ^ static_cast<std::uint8_t>(rhs[i]);

    return result == 0;
}

bool CTInsensitiveStringCompare(std::string_view lhs, std::string_view rhs) noexcept
{
    if(lhs.size() != rhs.size())
        return false;

    std::uint8_t result = 0;
    for(std::size_t i = 0; i < lhs.size(); ++i)
        result |= ToLowerAscii(static_cast<std::uint8_t>(lhs[i])) ^ ToLowerAscii(static_cast<std::uint8_t>(rhs[i]));

    return result == 0;
}

bool InsensitiveStringCompare(std::string_view lhs, std::string_view rhs) noexcept
{
    if(lhs.size() != rhs.size())
        return false;

    for(std::size_t i = 0; i < lhs.size(); ++i) {
        if(ToLowerAscii(static_cast<std::uint8_t>(lhs[i])) != ToLowerAscii(static_cast<std::uint8_t>(rhs[i])))
            return false;
    }

    return true;
}

// vvv Normalizations vvv
bool NormalizeURIPathInplace(std::string_view& path) noexcept
{
    if(path.empty())
        return false;

    char* buf = const_cast<char*>(path.data());
    std::size_t len = path.size();
    std::size_t out = 0;
    std::size_t i = 0;

    while(i < len) {
        // Collapse multiple slashes
        if(buf[i] == '/' && out > 0 && buf[out - 1] == '/') {
            ++i;
            continue;
        }

        // Handle dot segments
        if(buf[i] == '.' && (i + 1 >= len || buf[i + 1] == '/' || buf[i + 1] == '\0')) {
            // Single dot, skip
            i += (i + 1 < len && buf[i + 1] == '/') ? 2 : 1;
            continue;
        }

        if(buf[i] == '.' && i + 1 < len && buf[i + 1] == '.' && (i + 2 >= len || buf[i + 2] == '/')) {
            // Double dot, go up one level
            if(out > 1) {
                --out;
                while(out > 0 && buf[out - 1] != '/')
                    --out;
            }

            i += (i + 2 < len && buf[i + 2] == '/') ? 3 : 2;
            continue;
        }

        buf[out++] = buf[i++];
    }

    if(out == 0) {
        buf[0] = '/';
        out = 1;
    }

    path = std::string_view(buf, out);
    return true;
}

std::string NormalizePathToIdentifier(std::string_view path, std::string_view prefix) noexcept
{
    std::string result;
    result.reserve(prefix.size() + path.size());
    result.append(prefix);

    for(char c : path) {
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            result += c;
        else
            result += '_';
    }

    return result;
}

bool DecodePercentInplace(std::string_view& buf) noexcept
{
    if(buf.empty())
        return true;

    char* data = const_cast<char*>(buf.data());
    std::size_t len = buf.size();
    std::size_t out = 0;

    for(std::size_t i = 0; i < len; ++i) {
        if(data[i] == '%' && i + 2 < len) {
            std::uint8_t hi = UInt8FromHexChar(static_cast<std::uint8_t>(data[i + 1]));
            std::uint8_t lo = UInt8FromHexChar(static_cast<std::uint8_t>(data[i + 2]));

            if(hi == 0xFF || lo == 0xFF)
                return false;

            data[out++] = static_cast<char>((hi << 4) | lo);
            i += 2;
        }
        else if(data[i] == '+')
            data[out++] = ' ';
        else
            data[out++] = data[i];
    }

    buf = std::string_view(data, out);
    return true;
}

// vvv Conversions vvv
std::uint8_t ToLowerAscii(std::uint8_t c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? (c | 0x20) : c;
}

std::string UInt64ToStr(std::uint64_t value, const std::string& fallback) noexcept
{
    char buf[21];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    return ec != std::errc() ? fallback : std::string(buf, ptr);
}

bool StrToUInt64(std::string_view str, std::uint64_t& out) noexcept
{
    if(str.empty())
        return false;

    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), out);
    return ec == std::errc() && ptr == str.data() + str.size();
}

bool StrToInt64(std::string_view str, std::int64_t& out) noexcept
{
    if(str.empty())
        return false;

    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), out);
    return ec == std::errc() && ptr == str.data() + str.size();
}

// vvv Trim vvv
void TrimInline(std::string& s) noexcept
{
    const auto start = s.find_first_not_of(" \t\n\r\f\v");
    if(start == std::string::npos) {
        s.clear();
        return;
    }

    const auto end = s.find_last_not_of(" \t\n\r\f\v");
    s.erase(end + 1);
    s.erase(0, start);
}

std::string_view TrimView(std::string_view sv) noexcept
{
    std::size_t start = 0;
    std::size_t end = sv.size();

    while(start < end && std::isspace(static_cast<unsigned char>(sv[start])))
        ++start;
    while(end > start && std::isspace(static_cast<unsigned char>(sv[end - 1])))
        --end;

    return sv.substr(start, end - start);
}

} // namespace StringUtils
} // namespace WFX::Utils