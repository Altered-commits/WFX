#ifndef WFX_SHARED_JSON_WRITER_HPP
#define WFX_SHARED_JSON_WRITER_HPP

#include "http/response.hpp"
#include <string_view>
#include <cstdint>
#include <charconv>

namespace WFX::Shared {

class JsonWriter {
public: // Ctors and Dtors
    explicit JsonWriter(Http::Response& res) : res_(res)
    {
        res_.Header("Content-Type", "application/json");
        Raw("{", 1);
    }

    ~JsonWriter()
    {
        if(!done_) {
            while(depth_)
                Close();

            res_.Commit();
            done_ = true;
        }
    }

    JsonWriter(const JsonWriter&) = delete;
    JsonWriter& operator=(const JsonWriter&) = delete;

public: // Main Functions
    void Obj(std::string_view key)
    {
        Comma();
        WKey(key);
        Raw("{", 1);
        closes_ &= ~(1u << depth_);
        ++depth_;
        ResetComma();
    }

    void Arr(std::string_view key)
    {
        Comma();
        WKey(key);
        Raw("[", 1);
        closes_ |= (1u << depth_);
        ++depth_;
        ResetComma();
    }

    void Obj()
    {
        Comma();
        Raw("{", 1);
        closes_ &= ~(1u << depth_);
        ++depth_;
        ResetComma();
    }

    void Arr()
    {
        Comma();
        Raw("[", 1);
        closes_ |= (1u << depth_);
        ++depth_;
        ResetComma();
    }

    void End()
    {
        Close();
    }

    void Write(std::string_view key, const Shared::UUID& v)
    {
        Comma();
        WKey(key);
        WStr({v.ToString().data, 36});
    }

    void Write(std::string_view key, std::string_view v)
    {
        Comma();
        WKey(key);
        WStr(v);
    }
    void Write(std::string_view key, const char* v)
    {
        Comma();
        WKey(key);
        WStr(v);
    }
    void Write(std::string_view key, std::int64_t v)
    {
        Comma();
        WKey(key);
        WInt(v);
    }
    void Write(std::string_view key, std::uint64_t v)
    {
        Comma();
        WKey(key);
        WUInt(v);
    }
    void Write(std::string_view key, double v)
    {
        Comma();
        WKey(key);
        WDbl(v);
    }
    void Write(std::string_view key, bool v)
    {
        Comma();
        WKey(key);
        WBool(v);
    }
    void Write(std::string_view key, std::nullptr_t)
    {
        Comma();
        WKey(key);
        WNull();
    }

    void Write(std::string_view key, std::int32_t v)
    {
        Write(key, static_cast<std::int64_t>(v));
    }
    void Write(std::string_view key, std::uint32_t v)
    {
        Write(key, static_cast<std::uint64_t>(v));
    }
    void Write(std::string_view key, std::int16_t v)
    {
        Write(key, static_cast<std::int64_t>(v));
    }
    void Write(std::string_view key, std::uint16_t v)
    {
        Write(key, static_cast<std::uint64_t>(v));
    }
    void Write(std::string_view key, std::int8_t v)
    {
        Write(key, static_cast<std::int64_t>(v));
    }
    void Write(std::string_view key, std::uint8_t v)
    {
        Write(key, static_cast<std::uint64_t>(v));
    }
    void Write(std::string_view key, float v)
    {
        Write(key, static_cast<double>(v));
    }

    void Push(const Shared::UUID& v)
    {
        Comma();
        WStr({v.ToString().data, 36});
    }

    void Push(std::string_view v)
    {
        Comma();
        WStr(v);
    }
    void Push(std::int64_t v)
    {
        Comma();
        WInt(v);
    }
    void Push(std::uint64_t v)
    {
        Comma();
        WUInt(v);
    }
    void Push(double v)
    {
        Comma();
        WDbl(v);
    }
    void Push(bool v)
    {
        Comma();
        WBool(v);
    }
    void Push(std::nullptr_t)
    {
        Comma();
        WNull();
    }

    void Push(std::int32_t v)
    {
        Push(static_cast<std::int64_t>(v));
    }
    void Push(std::uint32_t v)
    {
        Push(static_cast<std::uint64_t>(v));
    }
    void Push(float v)
    {
        Push(static_cast<double>(v));
    }

private: // Helper Functions
    void Close()
    {
        --depth_;
        Raw((closes_ >> depth_) & 1u ? "]" : "}", 1);
    }

    void Comma()
    {
        std::uint16_t bit = 1u << depth_;
        if(commas_ & bit)
            Raw(",", 1);

        commas_ |= bit;
    }

    void ResetComma()
    {
        commas_ &= ~(1u << depth_);
    }

    void WKey(std::string_view k)
    {
        Raw("\"", 1);
        Escaped(k);
        Raw("\":", 2);
    }
    void WStr(std::string_view v)
    {
        Raw("\"", 1);
        Escaped(v);
        Raw("\"", 1);
    }

    void WInt(std::int64_t v)
    {
        char b[20];
        auto [e, _] = std::to_chars(b, b + 20, v);
        Raw(b, e - b);
    }
    void WUInt(std::uint64_t v)
    {
        char b[20];
        auto [e, _] = std::to_chars(b, b + 20, v);
        Raw(b, e - b);
    }
    void WDbl(double v)
    {
        char b[32];
        auto [e, _] = std::to_chars(b, b + 32, v);
        Raw(b, e - b);
    }
    void WBool(bool v)
    {
        v ? Raw("true", 4) : Raw("false", 5);
    }
    void WNull()
    {
        Raw("null", 4);
    }

    void Raw(const char* d, std::size_t l)
    {
        res_.Write({d, l});
    }

    void Escaped(std::string_view s)
    {
        const char* p = s.data();
        const char* e = p + s.size();
        const char* start = p;

        while(p != e) {
            const char* esc = nullptr;
            std::size_t el = 0;

            switch(*p) {
                case '"':
                    esc = "\\\"";
                    el = 2;
                    break;
                case '\\':
                    esc = "\\\\";
                    el = 2;
                    break;
                case '\n':
                    esc = "\\n";
                    el = 2;
                    break;
                case '\r':
                    esc = "\\r";
                    el = 2;
                    break;
                case '\t':
                    esc = "\\t";
                    el = 2;
                    break;
                case '\b':
                    esc = "\\b";
                    el = 2;
                    break;
                case '\f':
                    esc = "\\f";
                    el = 2;
                    break;
                default:
                    break;
            }

            if(esc) {
                if(p > start)
                    Raw(start, p - start);

                Raw(esc, el);
                start = p + 1;
            }

            ++p;
        }

        if(p > start)
            Raw(start, p - start);
    }

private: // Storage
    Http::Response& res_;
    std::uint16_t commas_ = 0;
    std::uint16_t closes_ = 0;
    std::uint8_t depth_ = 1;
    bool done_ = false;
};

} // namespace WFX::Shared

#endif // WFX_SHARED_JSON_WRITER_HPP