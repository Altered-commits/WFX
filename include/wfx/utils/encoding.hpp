// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_UTILS_ENCODING_HPP
#define WFX_INC_WFX_UTILS_ENCODING_HPP

// -----------------------------------------------------------------------
// wfx/utils/encoding.hpp
// Base64 / Hex / URL encoding, pure user-space, no engine ABI involved
//
// Provides:
//   WFX::Base64Encode(data, urlSafe = false, padded = true) / Base64Decode(data)
//   WFX::HexEncode(data, upper = false) / HexDecode(data)
//   WFX::UrlEncode(data) / UrlDecode(data)     : percent-encoding, RFC 3986
//
// Decode functions return {false, {}}; check the bool before using the result
//
// Base64 and hex also take a std::span to write into instead of allocating,
// returning {ok, written}. Base64EncodedLen / Base64DecodedMaxLen /
// HexEncodedLen / HexDecodedLen size that buffer
// -----------------------------------------------------------------------

#include "wfx/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace WFX {

namespace Detail {

inline constexpr char BASE64_STD_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline constexpr char BASE64_URL_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
inline constexpr char HEX_LOWER[] = "0123456789abcdef";
inline constexpr char HEX_UPPER[] = "0123456789ABCDEF";

// Per-byte properties, one table instead of three since all three are keyed by the same ASCII
// index anyway; base64/hex are 0xFF when the byte isn't a digit in that alphabet
struct CharInfo {
    std::uint8_t base64;
    std::uint8_t hex;
    bool urlSafe; // RFC 3986 unreserved set: ALPHA / DIGIT / '-' / '.' / '_' / '~'
};

// Built at compile time from the alphabets above, so all 256 bytes are covered and the table can't
// drift from them. Base64 accepts both alphabets ('+/' and '-_') in one table
inline constexpr std::array<CharInfo, 256> CHAR_TABLE = [] {
    std::array<CharInfo, 256> table{};
    for(auto& info : table)
        info = {0xFF, 0xFF, false};

    for(std::uint8_t i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(BASE64_STD_ALPHABET[i])].base64 = i;
        table[static_cast<unsigned char>(BASE64_URL_ALPHABET[i])].base64 = i;
    }

    for(std::uint8_t i = 0; i < 16; ++i) {
        table[static_cast<unsigned char>(HEX_LOWER[i])].hex = i;
        table[static_cast<unsigned char>(HEX_UPPER[i])].hex = i;
    }

    for(unsigned c = 0; c < 256; ++c)
        table[c].urlSafe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                           c == '.' || c == '_' || c == '~';

    return table;
}();

} // namespace Detail

// vvv Lengths vvv
// What the out-buffer forms below need room for
constexpr std::size_t Base64EncodedLen(std::size_t bytes, bool padded = true) noexcept
{
    return padded ? ((bytes + 2) / 3) * 4 : (bytes * 4 + 2) / 3;
}

// An upper bound: padding and a short final group only make the real length smaller
constexpr std::size_t Base64DecodedMaxLen(std::size_t chars) noexcept
{
    return chars * 3 / 4;
}

constexpr std::size_t HexEncodedLen(std::size_t bytes) noexcept
{
    return bytes * 2;
}

constexpr std::size_t HexDecodedLen(std::size_t chars) noexcept
{
    return chars / 2;
}

// vvv Base64 vvv
// Writes into out instead of allocating: {true, chars written}, or {false, 0} when out is
// shorter than Base64EncodedLen(data.size(), padded)
inline std::pair<bool, std::size_t> Base64Encode(std::string_view data, std::span<char> out, bool urlSafe = false,
                                                 bool padded = true) noexcept
{
    const std::size_t len = data.size();
    if(out.size() < Base64EncodedLen(len, padded))
        return {false, 0};

    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const char* alphabet = urlSafe ? Detail::BASE64_URL_ALPHABET : Detail::BASE64_STD_ALPHABET;
    std::size_t i = 0;
    std::size_t o = 0;

    while(i + 3 <= len) {
        const auto chunk = static_cast<std::uint32_t>((p[i] << 16) | (p[i + 1] << 8) | p[i + 2]);
        out[o++] = alphabet[(chunk >> 18) & 0x3F];
        out[o++] = alphabet[(chunk >> 12) & 0x3F];
        out[o++] = alphabet[(chunk >> 6) & 0x3F];
        out[o++] = alphabet[chunk & 0x3F];
        i += 3;
    }

    const std::size_t rem = len - i;
    if(rem == 1) {
        const auto chunk = static_cast<std::uint32_t>(p[i] << 16);
        out[o++] = alphabet[(chunk >> 18) & 0x3F];
        out[o++] = alphabet[(chunk >> 12) & 0x3F];
        if(padded) {
            out[o++] = '=';
            out[o++] = '=';
        }
    }
    else if(rem == 2) {
        const auto chunk = static_cast<std::uint32_t>((p[i] << 16) | (p[i + 1] << 8));
        out[o++] = alphabet[(chunk >> 18) & 0x3F];
        out[o++] = alphabet[(chunk >> 12) & 0x3F];
        out[o++] = alphabet[(chunk >> 6) & 0x3F];
        if(padded)
            out[o++] = '=';
    }

    return {true, o};
}

inline String Base64Encode(std::string_view data, bool urlSafe = false, bool padded = true)
{
    // The buffer is measured by the very function the span overload checks it against, so coming
    // up short is impossible. Sizing the result by what was actually written keeps that an
    // invariant rather than an assumption, and leaves nothing to a caller on the unreachable path
    String out(Base64EncodedLen(data.size(), padded), '\0');
    const auto [ok, written] = Base64Encode(data, std::span<char>{out.data(), out.size()}, urlSafe, padded);
    out.resize(ok ? written : 0);

    return out;
}

// Writes into out instead of allocating: {true, bytes written}, or {false, 0} on an invalid
// character or when out runs out of room (Base64DecodedMaxLen(data.size()) always suffices).
// Trailing '=' is skipped rather than required, so padded and unpadded input both work
// regardless of which alphabet produced them
inline std::pair<bool, std::size_t> Base64Decode(std::string_view data, std::span<std::uint8_t> out) noexcept
{
    std::size_t len = data.size();
    while(len > 0 && data[len - 1] == '=')
        --len;

    std::size_t o = 0;
    std::uint32_t buf = 0;
    int bits = 0;

    for(std::size_t i = 0; i < len; ++i) {
        const std::uint8_t v = Detail::CHAR_TABLE[static_cast<std::uint8_t>(data[i])].base64;
        if(v == 0xFF)
            return {false, 0};

        buf = (buf << 6) | v;
        bits += 6;

        if(bits >= 8) {
            if(o == out.size())
                return {false, 0};

            bits -= 8;
            out[o++] = static_cast<std::uint8_t>((buf >> bits) & 0xFF);
        }
    }

    return {true, o};
}

inline std::pair<bool, Vector<std::uint8_t>> Base64Decode(std::string_view data)
{
    Vector<std::uint8_t> out(Base64DecodedMaxLen(data.size()));
    const auto [ok, written] = Base64Decode(data, out);
    if(!ok)
        return {false, {}};

    out.resize(written);
    return {true, std::move(out)};
}

// vvv Hex vvv
// Writes into out instead of allocating: {true, chars written}, or {false, 0} when out is
// shorter than HexEncodedLen(data.size())
inline std::pair<bool, std::size_t> HexEncode(std::string_view data, std::span<char> out, bool upper = false) noexcept
{
    if(out.size() < HexEncodedLen(data.size()))
        return {false, 0};

    const char* table = upper ? Detail::HEX_UPPER : Detail::HEX_LOWER;
    for(std::size_t i = 0; i < data.size(); ++i) {
        const auto byte = static_cast<std::uint8_t>(data[i]);
        out[i * 2] = table[byte >> 4];
        out[i * 2 + 1] = table[byte & 0x0F];
    }

    return {true, HexEncodedLen(data.size())};
}

inline String HexEncode(std::string_view data, bool upper = false)
{
    // Same as Base64Encode above: sized by the function the span overload measures against, and
    // resized to what it wrote so the impossible short write cannot reach a caller as padding
    String out(HexEncodedLen(data.size()), '\0');
    const auto [ok, written] = HexEncode(data, std::span<char>{out.data(), out.size()}, upper);
    out.resize(ok ? written : 0);

    return out;
}

// Writes into out instead of allocating: {true, bytes written}, or {false, 0} on an odd length,
// an invalid character, or out shorter than HexDecodedLen(data.size())
inline std::pair<bool, std::size_t> HexDecode(std::string_view data, std::span<std::uint8_t> out) noexcept
{
    if(data.size() % 2 != 0 || out.size() < HexDecodedLen(data.size()))
        return {false, 0};

    for(std::size_t i = 0; i < data.size(); i += 2) {
        const std::uint8_t hi = Detail::CHAR_TABLE[static_cast<std::uint8_t>(data[i])].hex;
        const std::uint8_t lo = Detail::CHAR_TABLE[static_cast<std::uint8_t>(data[i + 1])].hex;
        if(hi == 0xFF || lo == 0xFF)
            return {false, 0};

        out[i / 2] = static_cast<std::uint8_t>((hi << 4) | lo);
    }

    return {true, HexDecodedLen(data.size())};
}

// {false, {}} on an odd length or an invalid character
inline std::pair<bool, Vector<std::uint8_t>> HexDecode(std::string_view data)
{
    Vector<std::uint8_t> out(HexDecodedLen(data.size()));
    const auto [ok, written] = HexDecode(data, out);
    if(!ok)
        return {false, {}};

    out.resize(written);
    return {true, std::move(out)};
}

// vvv URL / percent-encoding vvv
inline String UrlEncode(std::string_view data)
{
    String out;
    out.reserve(data.size());

    for(const char c : data) {
        const auto byte = static_cast<std::uint8_t>(c);
        if(Detail::CHAR_TABLE[byte].urlSafe)
            out.push_back(c);
        else {
            out.push_back('%');
            out.push_back(Detail::HEX_UPPER[byte >> 4]);
            out.push_back(Detail::HEX_UPPER[byte & 0x0F]);
        }
    }

    return out;
}

// {false, {}} on a truncated/invalid '%XX' escape
inline std::pair<bool, String> UrlDecode(std::string_view data)
{
    String out;
    out.reserve(data.size());

    for(std::size_t i = 0; i < data.size(); ++i) {
        if(data[i] == '%') {
            if(i + 2 >= data.size())
                return {false, {}};

            const std::uint8_t hi = Detail::CHAR_TABLE[static_cast<std::uint8_t>(data[i + 1])].hex;
            const std::uint8_t lo = Detail::CHAR_TABLE[static_cast<std::uint8_t>(data[i + 2])].hex;
            if(hi == 0xFF || lo == 0xFF)
                return {false, {}};

            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        }
        else if(data[i] == '+')
            out.push_back(' '); // form-encoded space, tolerated alongside strict '%20'
        else
            out.push_back(data[i]);
    }

    return {true, std::move(out)};
}

} // namespace WFX

#endif // WFX_INC_WFX_UTILS_ENCODING_HPP
