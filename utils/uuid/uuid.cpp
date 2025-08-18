#include "uuid.hpp"

#include "utils/backport/string.hpp"

namespace WFX::Utils {

bool UUID::FromString(std::string_view str, UUID& out)
{
    if(str.size() != 36) return false;

    static constexpr int dashPos[] = {8, 13, 18, 23};
    int dashIdx = 0, byteIdx = 0;

    for(int i = 0; i < 36;) {
        if(dashIdx < 4 && i == dashPos[dashIdx]) {
            if(str[i] != '-') return false;
            ++i; ++dashIdx;
        }

        uint8_t hi = UInt8FromHexChar(str[i]);
        uint8_t lo = UInt8FromHexChar(str[i + 1]);
        if(hi == 255 || lo == 255) return false;

        out.bytes[byteIdx++] = (hi << 4) | lo;
        i += 2;
    }

    return byteIdx == 16;
}

std::string UUID::ToString() const
{
    static constexpr char hex[] = "0123456789abcdef";
    char buf[37];

    int j = 0;
    for(int i = 0; i < 16; ++i) {
        if(i == 4 || i == 6 || i == 8 || i == 10) buf[j++] = '-';
        buf[j++] = hex[(bytes[i] >> 4) & 0xF];
        buf[j++] = hex[bytes[i] & 0xF];
    }

    return std::string(buf, 36);
}

bool UUID::operator==(const UUID& other) const
{
    return std::memcmp(bytes, other.bytes, 16) == 0;
}

} // namespace WFX::Utils
