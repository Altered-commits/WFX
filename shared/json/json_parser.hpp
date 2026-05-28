// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_JSON_PARSER_HPP
#define WFX_SHARED_JSON_PARSER_HPP

#include "json_object.hpp"
#include "shared/abis/string_view.hpp"
#include <string_view>
#include <cstdint>
#include <cstring>
#include <charconv>
#include <bit>

namespace WFX::Shared {

// vvv Parse result vvv
//
// error  : nullptr on success, static string literal on failure
// offset : byte offset into input where the failure occurred
struct JsonParseResult {
    JsonObject object;
    const char* error = nullptr;
    std::size_t offset = 0;

    bool IsValid() const noexcept
    {
        return error == nullptr;
    }
};

// vvv Internal parser, not for direct use vvv
//
// isView controls string value storage only:
//   true  : parsed string values are stored as STR_VIEW (zero-copy, src must outlive object)
//   false : parsed string values are copied into the store as STR_OWN (safe for temporary buffers)
//
struct JsonParser {
public: // vvv Primitives vvv
    bool Ok() const noexcept
    {
        return err == nullptr;
    }
    bool End() const noexcept
    {
        return pos >= len;
    }
    char Peek() const noexcept
    {
        return pos < len ? src[pos] : '\0';
    }

    void Fail(const char* msg) noexcept
    {
        if(!err) {
            err = msg;
            errOff = pos;
        }
    }

    void SkipWS() noexcept
    {
        while(pos < len) {
            char c = src[pos];
            if(c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;

            ++pos;
        }
    }

    bool Expect(char c) noexcept
    {
        SkipWS();

        if(Peek() != c) {
            Fail("unexpected token");
            return false;
        }

        ++pos;
        return true;
    }

    // Consume consecutive ASCII digits, returns false if none present
    bool SkipDigits() noexcept
    {
        if(pos >= len || src[pos] < '0' || src[pos] > '9')
            return false;

        while(pos < len && src[pos] >= '0' && src[pos] <= '9')
            ++pos;

        return true;
    }

public: // vvv Unicode vvv
    // Decode 4 hex digits at src[pos], advance pos by 4, return codepoint or -1
    std::int32_t DecodeHex4() noexcept
    {
        if(pos + 4 > len) {
            Fail("truncated \\u escape");
            return -1;
        }

        std::uint32_t cp = 0;

        for(int i = 0; i < 4; ++i) {
            char c = src[pos++];
            std::uint32_t d;

            if(c >= '0' && c <= '9')
                d = c - '0';
            else if(c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if(c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else {
                Fail("invalid hex digit in \\u escape");
                return -1;
            }

            cp = (cp << 4) | d;
        }

        return static_cast<std::int32_t>(cp);
    }

    // Encode codepoint to UTF-8 into out, return byte count
    static int EncodeUTF8(std::uint32_t cp, char* out) noexcept
    {
        if(cp < 0x80) {
            out[0] = static_cast<char>(cp);
            return 1;
        }

        if(cp < 0x800) {
            out[0] = static_cast<char>(0xC0 | (cp >> 6));
            out[1] = static_cast<char>(0x80 | (cp & 0x3F));
            return 2;
        }

        if(cp < 0x10000) {
            out[0] = static_cast<char>(0xE0 | (cp >> 12));
            out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out[2] = static_cast<char>(0x80 | (cp & 0x3F));
            return 3;
        }

        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        return 4;
    }

public: // vvv String scanning vvv
    // Scan src[pos...] for the end of a JSON string, stops at closing quote or first backslash
    // Sets end to that position, hasEscape to true if backslash found
    // Returns false on control character or unterminated string
    bool ScanString(std::size_t& end, bool& hasEscape) noexcept
    {
        for(std::size_t i = pos; i < len; ++i) {
            unsigned char c = static_cast<unsigned char>(src[i]);

            if(c < 0x20) {
                Fail("control character in string");
                return false;
            }

            if(src[i] == '\\') {
                hasEscape = true;
                end = i;
                return true;
            }
            if(src[i] == '"') {
                hasEscape = false;
                end = i;
                return true;
            }
        }

        Fail("unterminated string");
        return false;
    }

    // Decode one escape sequence, backslash already consumed
    // Returns false on error
    bool DecodeEscape(char* out, std::uint32_t& outLen) noexcept
    {
        if(pos >= len) {
            Fail("truncated escape");
            return false;
        }

        char esc = src[pos++];

        switch(esc) {
            case '"':
                out[outLen++] = '"';
                return true;
            case '\\':
                out[outLen++] = '\\';
                return true;
            case '/':
                out[outLen++] = '/';
                return true;
            case 'b':
                out[outLen++] = '\b';
                return true;
            case 'f':
                out[outLen++] = '\f';
                return true;
            case 'n':
                out[outLen++] = '\n';
                return true;
            case 'r':
                out[outLen++] = '\r';
                return true;
            case 't':
                out[outLen++] = '\t';
                return true;
            case 'u': {
                std::int32_t cp = DecodeHex4();
                if(cp < 0)
                    return false;

                if(cp >= 0xD800 && cp <= 0xDBFF) {
                    // High surrogate, must be followed by low surrogate \uXXXX
                    if(pos + 1 >= len || src[pos] != '\\' || src[pos + 1] != 'u') {
                        Fail("missing low surrogate");
                        return false;
                    }

                    pos += 2;

                    std::int32_t low = DecodeHex4();
                    if(low < 0)
                        return false;

                    if(low < 0xDC00 || low > 0xDFFF) {
                        Fail("invalid low surrogate");
                        return false;
                    }

                    std::uint32_t full = 0x10000u + ((static_cast<std::uint32_t>(cp) - 0xD800u) << 10) +
                                         (static_cast<std::uint32_t>(low) - 0xDC00u);

                    outLen += EncodeUTF8(full, out + outLen);
                }
                else if(cp >= 0xDC00 && cp <= 0xDFFF) {
                    Fail("unexpected low surrogate");
                    return false;
                }
                else
                    outLen += EncodeUTF8(static_cast<std::uint32_t>(cp), out + outLen);

                return true;
            }
            default:
                Fail("invalid escape sequence");
                return false;
        }
    }

public: // vvv String parsing vvv
    // Slow path: escape sequences present, always allocates into store
    // Even in view mode, escaped bytes differ from src so zero-copy is impossible
    // Returns false on error
    bool ParseStringEscaped(JsonStore* s, JsonNode& n) noexcept
    {
        std::uint32_t capacity = static_cast<std::uint32_t>(len - pos) + 4;
        std::uint32_t strOff = s->AllocStr(nullptr, capacity);

        if(strOff == JSON_NIL) {
            Fail("out of memory");
            return false;
        }

        char* out = s->strs + strOff;
        std::uint32_t outLen = 0;

        while(pos < len) {
            char c = src[pos];

            if(c == '"') {
                ++pos;
                break;
            }

            if(static_cast<unsigned char>(c) < 0x20) {
                Fail("control character in string");
                return false;
            }

            if(c != '\\') {
                out[outLen++] = c;
                ++pos;
                continue;
            }

            ++pos; // consume backslash

            if(!DecodeEscape(out, outLen))
                return false;
        }

        out[outLen] = '\0';
        s->strLen = strOff + outLen + 1; // patch strLen, capacity may be larger

        n.tag = JsonTag::STR_OWN;
        n.u32a() = strOff;
        n.u32c() = outLen;
        return true;
    }

    // Parse a JSON string into node, opening quote not yet consumed
    // Fast path (no escapes):
    //   isView = true  : STR_VIEW, zero-copy pointer into src (src must outlive the object)
    //   isView = false : STR_OWN, copied into store
    // Slow path (escapes present): always STR_OWN regardless of isView
    bool ParseString(JsonStore* s, JsonNode& n) noexcept
    {
        if(!Expect('"'))
            return false;

        std::size_t end = pos;
        bool hasEscape = false;

        if(!ScanString(end, hasEscape))
            return false;
        if(hasEscape)
            return ParseStringEscaped(s, n);

        // Fast path: no escape sequences in this string
        std::uint32_t slen = static_cast<std::uint32_t>(end - pos);

        if(isView) {
            // Zero-copy: pack pointer into u64a, store length in u32c
            // Must memcpy from a const char* local, &src[pos] is the char's address
            n.tag = JsonTag::STR_VIEW;
            const char* ptr = src + pos;
            std::memcpy(&n.u64a, &ptr, 8);
            n.u32c() = slen;
        }
        else {
            std::uint32_t off = s->AllocStr(src + pos, slen);

            if(off == JSON_NIL) {
                Fail("out of memory");
                return false;
            }

            n.tag = JsonTag::STR_OWN;
            n.u32a() = off;
            n.u32c() = slen;
        }

        pos = end + 1; // skip closing "
        return true;
    }

public: // vvv Number parsing vvv
    // After return, src[start..pos] is the raw number text
    // Sets isFloat if decimal point or exponent found
    // Returns false on malformed input
    bool ScanNumber(bool& isFloat) noexcept
    {
        if(!SkipDigits()) {
            Fail("invalid number");
            return false;
        }

        if(pos < len && src[pos] == '.') {
            isFloat = true;
            ++pos;

            if(!SkipDigits()) {
                Fail("invalid number");
                return false;
            }
        }

        if(pos < len && (src[pos] == 'e' || src[pos] == 'E')) {
            isFloat = true;
            ++pos;

            if(pos < len && (src[pos] == '+' || src[pos] == '-'))
                ++pos;

            if(!SkipDigits()) {
                Fail("invalid number");
                return false;
            }
        }

        return true;
    }

    bool ParseNumber(JsonNode& n) noexcept
    {
        std::size_t start = pos;
        bool isNeg = Peek() == '-';
        bool isFloat = false;

        if(isNeg)
            ++pos;
        if(!ScanNumber(isFloat))
            return false;

        const char* begin = src + start;
        const char* end = src + pos;

        if(isFloat) {
            double v;
            if(std::from_chars(begin, end, v).ec != std::errc{}) {
                Fail("invalid number");
                return false;
            }

            n.tag = JsonTag::DOUBLE;
            std::memcpy(&n.u64a, &v, 8);
        }
        else if(isNeg) {
            std::int64_t v;
            if(std::from_chars(begin, end, v).ec != std::errc{}) {
                Fail("invalid number");
                return false;
            }

            n.tag = JsonTag::INT64;
            std::memcpy(&n.u64a, &v, 8);
        }
        else {
            std::uint64_t v;
            if(std::from_chars(begin, end, v).ec != std::errc{}) {
                Fail("invalid number");
                return false;
            }

            n.tag = JsonTag::UINT64;
            std::memcpy(&n.u64a, &v, 8);
        }

        return true;
    }

public: // vvv Value vvv
    // Parse any JSON value, writing result into an existing JsonRef
    // The JsonRef is provided by the caller (from PushBack or GetOrCreate on the parent)
    // This function only decides what type the value is and fills the node
    bool ParseValue(JsonRef& r) noexcept
    {
        SkipWS();

        if(!Ok() || End()) {
            Fail("unexpected end of input");
            return false;
        }

        char c = Peek();
        JsonNode& n = r.NMut();

        if(c == '"')
            return ParseString(r.s_, n);
        if(c == '{')
            return ParseObject(r);
        if(c == '[')
            return ParseArray(r);

        if(c == 't') {
            if(pos + 4 > len || std::memcmp(src + pos, "true", 4) != 0) {
                Fail("invalid literal");
                return false;
            }

            pos += 4;
            n.tag = JsonTag::BOOL;
            n.u64a = 1;
            return true;
        }

        if(c == 'f') {
            if(pos + 5 > len || std::memcmp(src + pos, "false", 5) != 0) {
                Fail("invalid literal");
                return false;
            }

            pos += 5;
            n.tag = JsonTag::BOOL;
            n.u64a = 0;
            return true;
        }

        if(c == 'n') {
            if(pos + 4 > len || std::memcmp(src + pos, "null", 4) != 0) {
                Fail("invalid literal");
                return false;
            }

            pos += 4;
            n.tag = JsonTag::EMPTY;
            return true;
        }

        if(c == '-' || (c >= '0' && c <= '9'))
            return ParseNumber(n);

        Fail("unexpected character");
        return false;
    }

public: // vvv Object and Array vvv
    // For each key, GetOrCreate copies the key into the store regardless of isView
    // Keys must always be owned, src may not outlive the JsonObject
    bool ParseObject(JsonRef r) noexcept
    {
        ++pos; // skip '{'
        if(++depth > maxDepth) {
            Fail("max depth exceeded");
            return false;
        }

        r.NMut().tag = JsonTag::OBJECT;
        r.NMut().u64a = 0;
        r.NMut().u64b = 0;

        SkipWS();
        if(Peek() == '}') {
            ++pos;
            --depth;
            return true;
        }

        while(Ok()) {
            SkipWS();

            if(Peek() != '"') {
                Fail("expected string key");
                return false;
            }

            std::size_t keyStart = ++pos; // skip opening "
            std::size_t end = pos;
            bool hasEscape = false;

            if(!ScanString(end, hasEscape))
                return false;

            JsonRef valRef{nullptr, JSON_NIL};

            if(!hasEscape) {
                // Fast path: key has no escape sequences, copy directly from src
                std::uint32_t klen = static_cast<std::uint32_t>(end - keyStart);
                pos = end + 1; // skip closing "
                valRef = r.GetOrCreate(src + keyStart, klen);
            }
            else {
                // Escaped key: rewind and parse the full string to get decoded bytes,-
                // -then insert. Always owned, escaped content differs from src bytes
                pos = keyStart - 1; // rewind to opening quote

                JsonNode tmp;
                tmp.tag = JsonTag::EMPTY;
                tmp.u64a = 0;
                tmp.u64b = 0;

                // Force copy for the key parse regardless of isView
                bool savedView = isView;
                isView = false;

                bool ok = ParseString(r.s_, tmp);
                isView = savedView;

                if(!ok)
                    return false;

                const char* kptr = r.s_->strs + tmp.u32a();
                std::uint32_t klen = tmp.u32c();
                valRef = r.GetOrCreate(kptr, klen);
            }

            if(!valRef.Valid()) {
                Fail("out of memory");
                return false;
            }

            if(!Expect(':'))
                return false;

            SkipWS();
            if(!ParseValue(valRef))
                return false;

            SkipWS();
            char next = Peek();

            if(next == '}') {
                ++pos;
                break;
            }

            if(next != ',') {
                Fail("expected ',' or '}'");
                return false;
            }

            ++pos;
        }

        --depth;
        return Ok();
    }

    // For each element, PushBack() gives a value JsonRef, then parse recursively into it
    bool ParseArray(JsonRef r) noexcept
    {
        ++pos; // skip '['
        if(++depth > maxDepth) {
            Fail("max depth exceeded");
            return false;
        }

        r.NMut().tag = JsonTag::ARRAY;
        r.NMut().u64a = 0;
        r.NMut().u64b = 0;

        SkipWS();
        if(Peek() == ']') {
            ++pos;
            --depth;
            return true;
        }

        while(Ok()) {
            JsonRef elem = r.PushBack();

            if(!elem.Valid()) {
                Fail("out of memory");
                return false;
            }

            SkipWS();
            if(!ParseValue(elem))
                return false;

            SkipWS();
            char next = Peek();

            if(next == ']') {
                ++pos;
                break;
            }

            if(next != ',') {
                Fail("expected ',' or ']'");
                return false;
            }

            ++pos;
        }

        --depth;
        return Ok();
    }

public: // vvv Entry point vvv
    // isView : controls string value storage only (not keys, which are always owned)
    //   true  : zero-copy STR_VIEW for string values, src must outlive the returned object
    //   false : STR_OWN copies for string values, safe for temporary buffers
    static JsonParseResult ParseImpl(std::string_view body, bool isView, std::uint32_t maxDepth) noexcept
    {
        JsonParseResult result;

        if(body.empty()) {
            result.error = "empty input";
            return result;
        }

        result.object = JsonObject::Init();

        if(!result.object.Valid()) {
            result.error = "out of memory";
            return result;
        }

        JsonParser p;
        p.src = body.data();
        p.len = body.size();
        p.pos = 0;
        p.depth = 0;
        p.maxDepth = maxDepth;
        p.isView = isView;
        p.err = nullptr;
        p.errOff = 0;

        p.SkipWS();

        if(p.Peek() != '{' && p.Peek() != '[') {
            result.error = "root must be object or array";
            result.offset = p.pos;
            return result;
        }

        // Root node is node 0, always allocated by JsonObject::Init()
        JsonRef root{result.object.s_, 0};

        bool ok = p.Peek() == '{' ? p.ParseObject(root) : p.ParseArray(root);

        if(!ok || !p.Ok()) {
            result.error = p.err ? p.err : "parse error";
            result.offset = p.errOff;
            return result;
        }

        p.SkipWS();

        if(!p.End()) {
            result.error = "trailing content after root";
            result.offset = p.pos;
            return result;
        }

        return result;
    }

private: // vvv State vvv
    const char* src;
    std::size_t len;
    std::size_t pos;
    std::uint32_t depth;
    std::uint32_t maxDepth;
    const char* err = nullptr;
    std::size_t errOff = 0;
    bool isView;
};

} // namespace WFX::Shared

#endif // WFX_SHARED_JSON_PARSER_HPP