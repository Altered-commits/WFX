// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_ENDPOINT_HELPER_HPP
#define WFX_INC_WFX_ENDPOINT_HELPER_HPP

#include "wfx/memory.hpp"
#include <cstdint>
#include <cstring>
#include <string_view>

// Small helpers shared between the protocol codecs built on wfx/endpoint/base.hpp
// (HttpEndpoint, SmtpEndpoint, ...). Kept out of the public WFX:: surface.
namespace WFX::EndpointDetail {

// Bounds-checked append-only cursor over a serialize() buffer. Append fails cleanly (false) on
// overflow, so the caller returns EpSerBufferTooSmall and the engine retries with a larger buffer.
class BufWriter {
public:
    BufWriter(char* buf, std::uint32_t cap) noexcept : buf_(buf), cap_(cap)
    {}

    bool Append(std::string_view s) noexcept
    {
        if(s.size() > cap_ - pos_)
            return false;

        std::memcpy(buf_ + pos_, s.data(), s.size());
        pos_ += static_cast<std::uint32_t>(s.size());
        return true;
    }

    bool Append(char c) noexcept
    {
        if(pos_ >= cap_)
            return false;

        buf_[pos_++] = c;
        return true;
    }

    std::uint32_t Pos() const noexcept
    {
        return pos_;
    }

private:
    char* buf_;
    std::uint32_t cap_;
    std::uint32_t pos_ = 0;
};

constexpr bool HasInjectionBytes(std::string_view s) noexcept
{
    for(const char c : s)
        if(c == '\r' || c == '\n' || c == '\0')
            return true;

    return false;
}

constexpr char ToLowerAscii(char c) noexcept
{
    auto uc = static_cast<unsigned char>(c);
    const unsigned char isUpper = static_cast<unsigned char>(uc - 'A') < 26;
    return static_cast<char>(uc | static_cast<unsigned char>(isUpper << 5));
}

constexpr bool InsensitiveEqual(std::string_view a, std::string_view b) noexcept
{
    if(a.size() != b.size())
        return false;

    for(std::size_t i = 0; i < a.size(); ++i)
        if(ToLowerAscii(a[i]) != ToLowerAscii(b[i]))
            return false;

    return true;
}

constexpr bool InsensitiveStartsWith(std::string_view s, std::string_view prefix) noexcept
{
    return s.size() >= prefix.size() && InsensitiveEqual(s.substr(0, prefix.size()), prefix);
}

enum class LineReadStatus : std::uint8_t { NEED_MORE, GOT_LINE, TOO_LONG };

// Reads one '\n'-terminated line starting at buf[pos]. Shared shape behind HTTP status/header
// lines and SMTP response lines.
inline LineReadStatus ReadLine(const char* buf, std::uint32_t len, std::uint32_t& pos, WFX::String& lineAcc,
                               std::uint32_t maxLineBytes, std::string_view& line) noexcept
{
    const void* nl = std::memchr(buf + pos, '\n', len - pos);
    if(!nl) {
        // No terminator yet: stash what we have and ask the caller for more bytes
        const std::uint32_t remaining = len - pos;
        if(lineAcc.size() + remaining > maxLineBytes)
            return LineReadStatus::TOO_LONG;

        lineAcc.append(buf + pos, remaining);
        pos = len;
        return LineReadStatus::NEED_MORE;
    }

    const auto lineLen = static_cast<std::uint32_t>(static_cast<const char*>(nl) - (buf + pos));
    if(!lineAcc.empty()) {
        if(lineAcc.size() + lineLen > maxLineBytes)
            return LineReadStatus::TOO_LONG;

        lineAcc.append(buf + pos, lineLen);
        line = lineAcc; // view into lineAcc: caller must not touch lineAcc before using line
    }
    else {
        if(lineLen > maxLineBytes)
            return LineReadStatus::TOO_LONG;

        line = std::string_view{buf + pos, lineLen};
    }

    if(!line.empty() && line.back() == '\r')
        line.remove_suffix(1);

    pos += lineLen + 1; // past the '\n'; caller clears lineAcc once done with line
    return LineReadStatus::GOT_LINE;
}

} // namespace WFX::EndpointDetail

#endif // WFX_INC_WFX_ENDPOINT_HELPER_HPP
