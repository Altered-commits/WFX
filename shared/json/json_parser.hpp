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
        return err_ == nullptr;
    }
    bool End() const noexcept
    {
        return pos_ >= len_;
    }
    char Peek() const noexcept
    {
        return pos_ < len_ ? src_[pos_] : '\0';
    }

    void Fail(const char* msg) noexcept
    {
        if(!err_) {
            err_ = msg;
            errOff_ = pos_;
        }
    }

    void SkipWS() noexcept
    {
        while(pos_ < len_) {
            const char c = src_[pos_];
            if(c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;

            ++pos_;
        }
    }

    bool Expect(char c) noexcept
    {
        SkipWS();

        if(Peek() != c) {
            Fail("unexpected token");
            return false;
        }

        ++pos_;
        return true;
    }

    // Consume consecutive ASCII digits, returns false if none present
    bool SkipDigits() noexcept
    {
        if(pos_ >= len_ || src_[pos_] < '0' || src_[pos_] > '9')
            return false;

        while(pos_ < len_ && src_[pos_] >= '0' && src_[pos_] <= '9')
            ++pos_;

        return true;
    }

public: // vvv Unicode vvv
    // Decode 4 hex digits at src[pos], advance pos by 4, return codepoint or -1
    std::int32_t DecodeHex4() noexcept
    {
        if(pos_ + 4 > len_) {
            Fail("truncated \\u escape");
            return -1;
        }

        std::uint32_t cp = 0;

        for(int i = 0; i < 4; ++i) {
            const char c = src_[pos_++];
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
        for(std::size_t i = pos_; i < len_; ++i) {
            const unsigned char c = static_cast<unsigned char>(src_[i]);

            if(c < 0x20) {
                Fail("control character in string");
                return false;
            }

            if(src_[i] == '\\') {
                hasEscape = true;
                end = i;
                return true;
            }
            if(src_[i] == '"') {
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
        if(pos_ >= len_) {
            Fail("truncated escape");
            return false;
        }

        const char esc = src_[pos_++];

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
                const std::int32_t cp = DecodeHex4();
                if(cp < 0)
                    return false;

                if(cp >= 0xD800 && cp <= 0xDBFF) {
                    // High surrogate, must be followed by low surrogate \uXXXX
                    if(pos_ + 1 >= len_ || src_[pos_] != '\\' || src_[pos_ + 1] != 'u') {
                        Fail("missing low surrogate");
                        return false;
                    }

                    pos_ += 2;

                    const std::int32_t low = DecodeHex4();
                    if(low < 0)
                        return false;

                    if(low < 0xDC00 || low > 0xDFFF) {
                        Fail("invalid low surrogate");
                        return false;
                    }

                    const std::uint32_t full = 0x10000u + ((static_cast<std::uint32_t>(cp) - 0xD800u) << 10) +
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
        const std::uint32_t capacity = static_cast<std::uint32_t>(len_ - pos_) + 4;
        const std::uint32_t strOff = s->AllocStr(nullptr, capacity);

        if(strOff == JSON_NIL) {
            Fail("out of memory");
            return false;
        }

        char* out = s->strs + strOff;
        std::uint32_t outLen = 0;

        while(pos_ < len_) {
            const char c = src_[pos_];

            if(c == '"') {
                ++pos_;
                break;
            }

            if(static_cast<unsigned char>(c) < 0x20) {
                Fail("control character in string");
                return false;
            }

            if(c != '\\') {
                out[outLen++] = c;
                ++pos_;
                continue;
            }

            ++pos_; // consume backslash

            if(!DecodeEscape(out, outLen))
                return false;
        }

        out[outLen] = '\0';
        s->strLen = strOff + outLen + 1; // patch strLen, capacity may be larger

        n.tag = JsonTag::STR_OWN;
        n.U32a() = strOff;
        n.U32c() = outLen;
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

        std::size_t end = pos_;
        bool hasEscape = false;

        if(!ScanString(end, hasEscape))
            return false;
        if(hasEscape)
            return ParseStringEscaped(s, n);

        // Fast path: no escape sequences in this string
        const std::uint32_t slen = static_cast<std::uint32_t>(end - pos_);

        if(isView_) {
            // Zero-copy: pack pointer into u64a, store length in U32c
            // Must memcpy from a const char* local, &src[pos] is the char's address
            n.tag = JsonTag::STR_VIEW;
            const char* ptr = src_ + pos_;

            // NOLINTNEXTLINE(bugprone-multi-level-implicit-pointer-conversion): intentional type pun, see above.
            std::memcpy(&n.u64a, &ptr, 8);
            n.U32c() = slen;
        }
        else {
            const std::uint32_t off = s->AllocStr(src_ + pos_, slen);

            if(off == JSON_NIL) {
                Fail("out of memory");
                return false;
            }

            n.tag = JsonTag::STR_OWN;
            n.U32a() = off;
            n.U32c() = slen;
        }

        pos_ = end + 1; // skip closing "
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

        if(pos_ < len_ && src_[pos_] == '.') {
            isFloat = true;
            ++pos_;

            if(!SkipDigits()) {
                Fail("invalid number");
                return false;
            }
        }

        if(pos_ < len_ && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            isFloat = true;
            ++pos_;

            if(pos_ < len_ && (src_[pos_] == '+' || src_[pos_] == '-'))
                ++pos_;

            if(!SkipDigits()) {
                Fail("invalid number");
                return false;
            }
        }

        return true;
    }

    bool ParseNumber(JsonNode& n) noexcept
    {
        const std::size_t start = pos_;
        const bool isNeg = Peek() == '-';
        bool isFloat = false;

        if(isNeg)
            ++pos_;
        if(!ScanNumber(isFloat))
            return false;

        const char* begin = src_ + start;
        const char* end = src_ + pos_;

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

        const char c = Peek();
        JsonNode& n = r.NMut();

        if(c == '"')
            return ParseString(r.s_, n);
        if(c == '{')
            return ParseObject(r);
        if(c == '[')
            return ParseArray(r);

        if(c == 't') {
            if(pos_ + 4 > len_ || std::memcmp(src_ + pos_, "true", 4) != 0) {
                Fail("invalid literal");
                return false;
            }

            pos_ += 4;
            n.tag = JsonTag::BOOL;
            n.u64a = 1;
            return true;
        }

        if(c == 'f') {
            if(pos_ + 5 > len_ || std::memcmp(src_ + pos_, "false", 5) != 0) {
                Fail("invalid literal");
                return false;
            }

            pos_ += 5;
            n.tag = JsonTag::BOOL;
            n.u64a = 0;
            return true;
        }

        if(c == 'n') {
            if(pos_ + 4 > len_ || std::memcmp(src_ + pos_, "null", 4) != 0) {
                Fail("invalid literal");
                return false;
            }

            pos_ += 4;
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
        ++pos_; // skip '{'
        if(++depth_ > maxDepth_) {
            Fail("max depth exceeded");
            return false;
        }

        r.NMut().tag = JsonTag::OBJECT;
        r.NMut().u64a = 0;
        r.NMut().u64b = 0;

        SkipWS();
        if(Peek() == '}') {
            ++pos_;
            --depth_;
            return true;
        }

        while(Ok()) {
            SkipWS();

            if(Peek() != '"') {
                Fail("expected string key");
                return false;
            }

            const std::size_t keyStart = ++pos_; // skip opening "
            std::size_t end = pos_;
            bool hasEscape = false;

            if(!ScanString(end, hasEscape))
                return false;

            JsonRef valRef{nullptr, JSON_NIL};

            if(!hasEscape) {
                // Fast path: key has no escape sequences, copy directly from src
                const std::uint32_t klen = static_cast<std::uint32_t>(end - keyStart);
                pos_ = end + 1; // skip closing "
                valRef = r.GetOrCreate(src_ + keyStart, klen);
            }
            else {
                // Escaped key: rewind and parse the full string to get decoded bytes, then insert.
                // Always owned, escaped content differs from src bytes.
                pos_ = keyStart - 1; // rewind to opening quote

                JsonNode tmp;
                tmp.tag = JsonTag::EMPTY;
                tmp.u64a = 0;
                tmp.u64b = 0;

                // Force copy for the key parse regardless of isView
                const bool savedView = isView_;
                isView_ = false;

                const bool ok = ParseString(r.s_, tmp);
                isView_ = savedView;

                if(!ok)
                    return false;

                // The decoded key already lives in the store (ParseStringEscaped wrote it there).
                // Insert it by offset, NOT by pointer: GetOrCreate would feed that strs-interior
                // pointer to AllocStr, which reallocs strs and then memcpy's from the freed
                // original (use-after-free), on top of storing the key a second time.
                valRef = r.GetOrCreateStored(tmp.U32a(), tmp.U32c());
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
            const char next = Peek();

            if(next == '}') {
                ++pos_;
                break;
            }

            if(next != ',') {
                Fail("expected ',' or '}'");
                return false;
            }

            ++pos_;
        }

        --depth_;
        return Ok();
    }

    // For each element, PushBack() gives a value JsonRef, then parse recursively into it
    bool ParseArray(JsonRef r) noexcept
    {
        ++pos_; // skip '['
        if(++depth_ > maxDepth_) {
            Fail("max depth exceeded");
            return false;
        }

        r.NMut().tag = JsonTag::ARRAY;
        r.NMut().u64a = 0;
        r.NMut().u64b = 0;

        SkipWS();
        if(Peek() == ']') {
            ++pos_;
            --depth_;
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
            const char next = Peek();

            if(next == ']') {
                ++pos_;
                break;
            }

            if(next != ',') {
                Fail("expected ',' or ']'");
                return false;
            }

            ++pos_;
        }

        --depth_;
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
        p.src_ = body.data();
        p.len_ = body.size();
        p.pos_ = 0;
        p.depth_ = 0;
        p.maxDepth_ = maxDepth;
        p.isView_ = isView;
        p.err_ = nullptr;
        p.errOff_ = 0;

        p.SkipWS();

        if(p.Peek() != '{' && p.Peek() != '[') {
            result.error = "root must be object or array";
            result.offset = p.pos_;
            return result;
        }

        // Root node is node 0, always allocated by JsonObject::Init()
        const JsonRef root{result.object.s_, 0};

        const bool ok = p.Peek() == '{' ? p.ParseObject(root) : p.ParseArray(root);

        if(!ok || !p.Ok()) {
            result.error = p.err_ ? p.err_ : "parse error";
            result.offset = p.errOff_;
            return result;
        }

        p.SkipWS();

        if(!p.End()) {
            result.error = "trailing content after root";
            result.offset = p.pos_;
            return result;
        }

        return result;
    }

private: // vvv State vvv
    const char* src_;
    std::size_t len_;
    std::size_t pos_;
    std::uint32_t depth_;
    std::uint32_t maxDepth_;
    const char* err_ = nullptr;
    std::size_t errOff_ = 0;
    bool isView_;
};

} // namespace WFX::Shared

#endif // WFX_SHARED_JSON_PARSER_HPP