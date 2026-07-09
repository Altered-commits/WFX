// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_FORMS_HPP
#define WFX_INC_FORMS_HPP

#include "fields.hpp"
#include "validators.hpp"
#include "sanitizers.hpp"
#include "renders.hpp"
#include "http/request.hpp"
#include <array>
#include <cctype>
#include <cstdint>
#include <string_view>

namespace WFX::Form {

// Inline, header-only (user .so builds can't link non-inline engine utils)
namespace Detail {

constexpr std::uint8_t HexNibble(std::uint8_t c) noexcept
{
    std::uint8_t lo = static_cast<std::uint8_t>(c - '0');
    std::uint8_t hi = static_cast<std::uint8_t>((c | 0x20) - 'a');
    bool isDigit = lo < 10;
    bool isHex = hi < 6;
    return isDigit ? lo : (isHex ? static_cast<std::uint8_t>(hi + 10) : std::uint8_t{0xFF});
}

inline std::uint8_t ToLowerAscii(std::uint8_t c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<std::uint8_t>(c | 0x20) : c;
}

inline bool InsensitiveEquals(std::string_view a, std::string_view b) noexcept
{
    if(a.size() != b.size())
        return false;

    for(std::size_t i = 0; i < a.size(); ++i)
        if(ToLowerAscii(static_cast<std::uint8_t>(a[i])) != ToLowerAscii(static_cast<std::uint8_t>(b[i])))
            return false;

    return true;
}

inline std::string_view TrimAscii(std::string_view sv) noexcept
{
    std::size_t start = 0;
    std::size_t end = sv.size();

    while(start < end && std::isspace(static_cast<unsigned char>(sv[start])))
        ++start;
    while(end > start && std::isspace(static_cast<unsigned char>(sv[end - 1])))
        --end;

    return sv.substr(start, end - start);
}

// Rejects on a malformed %XX escape instead of silently passing bad bytes through
inline bool DecodePercentInplace(std::string_view& buf) noexcept
{
    if(buf.empty())
        return true;

    char* data = const_cast<char*>(buf.data());
    std::size_t len = buf.size();
    std::size_t out = 0;

    for(std::size_t i = 0; i < len; ++i) {
        if(data[i] == '%' && i + 2 < len) {
            std::uint8_t hi = HexNibble(static_cast<std::uint8_t>(data[i + 1]));
            std::uint8_t lo = HexNibble(static_cast<std::uint8_t>(data[i + 2]));

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

} // namespace Detail

// vvv Field Builders vvv
template <typename Rule> class FieldBuilder {
public: // Helper Aliases
    using DescType = FieldDesc<Rule>;
    using PairType = std::pair<std::string_view, DescType>;

public: // Constructor
    constexpr FieldBuilder(const char* name, Rule rule)
        : pair_{std::string_view{name}, FieldDesc<Rule>{rule, DefaultValidatorFor(rule), DefaultSanitizerFor(rule)}}
    {}

public: // Helper Functions
    constexpr FieldBuilder& CustomValidator(ValidatorFn v) &
    {
        pair_.second.validator = v;
        return *this;
    }

    constexpr FieldBuilder&& CustomValidator(ValidatorFn v) &&
    {
        pair_.second.validator = v;
        return std::move(*this);
    }

    constexpr FieldBuilder& CustomSanitizer(SanitizerFn<typename DescType::RawType> s) &
    {
        pair_.second.sanitizer = s;
        return *this;
    }

    constexpr FieldBuilder&& CustomSanitizer(SanitizerFn<typename DescType::RawType> s) &&
    {
        pair_.second.sanitizer = s;
        return std::move(*this);
    }

public: // Getters
    constexpr std::string_view GetName() const&
    {
        return pair_.first;
    }
    constexpr DescType&& GetDesc() &&
    {
        return std::move(pair_.second);
    }

private: // Storage
    PairType pair_;
};

template <typename Rule> constexpr auto Field(const char* name, Rule rule)
{
    return FieldBuilder{name, std::move(rule)};
}

// vvv Wrapper for sanitized value vvv
template <typename T> struct CleanedValue {
    T value{};
    bool present = false;
};

// vvv Tuple Builder vvv
template <typename... Fields> struct CleanedTupleFor {
    using Type = std::tuple<CleanedValue<typename Fields::RawType>...>;
};

// vvv Error Handling vvv
enum class FormError : std::uint8_t { NONE, UNSUPPORTED_CONTENT_TYPE, MALFORMED, CLEAN_FAILED };

// vvv Main shit vvv
template <typename... Fields> struct FormSchema {
    /*
     * Fields is 'FieldBuilder' returned by 'Field' function
     */
public: // Aliases
    static constexpr std::size_t FieldCount = sizeof...(Fields);

    // Stored
    using FieldsTuple = std::tuple<typename Fields::DescType...>;
    using NamesArray = std::array<std::string_view, FieldCount>;

    // Helper
    using CleanedType = typename CleanedTupleFor<typename Fields::DescType...>::Type;
    using InputType = std::array<std::string_view, FieldCount>;

public:
    template <std::size_t N>
    constexpr FormSchema(const char (&formName)[N], Fields&&... f)
        : formName{formName, N - 1}, fieldNames{f.GetName()...}, fieldRules{std::move(f).GetDesc()...}
    {
        static_assert(N > 1, "FormSchema.formName cannot be empty");

        // Avoid too many reallocs
        preRenderedFields.reserve(FieldCount * 100);

        // Render each field
        RenderFields(std::make_index_sequence<FieldCount>{});
    }

public: // Main Functions
    // Auto select the parsing type looking at the header
    FormError Parse(Http::Request req, CleanedType& out) const
    {
        std::string_view contentType;
        if(!req.GetHeader("Content-Type", contentType))
            return FormError::UNSUPPORTED_CONTENT_TYPE;

        // Content-Type can contain multiple fields seperated by ';'
        // What we need is the initial one
        auto ct = Detail::TrimAscii(contentType.substr(0, contentType.find(';')));

        // In memory simple form
        if(Detail::InsensitiveEquals(ct, "application/x-www-form-urlencoded"))
            return ParseStatic(req.Body(), out);

        // Other types of forms are not supported for now
        return FormError::UNSUPPORTED_CONTENT_TYPE;
    }

    // Parse small, in memory form (like application/x-www-form-urlencoded)
    FormError ParseStatic(std::string_view body, CleanedType& out) const
    {
        InputType input{};
        if(!SplitIntoArray(body, input))
            return FormError::MALFORMED;

        return (!Clean(input, out, std::make_index_sequence<FieldCount>{}) ? FormError::CLEAN_FAILED : FormError::NONE);
    }

    // Returns view to pre-rendered fields. NOTE: <form></form> needs to be written by user
    std::string_view Render() const
    {
        return preRenderedFields;
    }

private: // Helper Functions
    bool SplitIntoArray(std::string_view body, InputType& out) const
    {
        std::size_t fieldIdx = 0;
        std::size_t pos = 0;

        while(pos <= body.size()) {
            std::size_t start = pos;
            std::size_t end = body.find('&', pos);
            if(end == std::string_view::npos)
                end = body.size();

            // More pairs than expected
            if(fieldIdx >= FieldCount)
                return false;

            auto kv = body.substr(start, end - start);
            auto eqPos = kv.find('=');
            // Missing '='
            if(eqPos == std::string_view::npos)
                return false;

            std::string_view key = kv.substr(0, eqPos);
            std::string_view value = kv.substr(eqPos + 1);

            // Check key matches the schema field at this index
            if(key != fieldNames[fieldIdx])
                return false;

            // Decode value in place
            if(!Detail::DecodePercentInplace(value))
                return false;

            out[fieldIdx++] = value;

            pos = end + 1;
        }

        return fieldIdx == FieldCount;
    }

    // Validate Then Sanitize
    template <typename Field>
    bool VTSField(const Field& fd, std::string_view in, CleanedValue<typename Field::RawType>& out) const
    {
        // Presence check FIRST
        if(in.empty()) {
            if(fd.rule.required)
                return false; // Missing required field

            // Optional field
            out.present = false;
            return true;
        }

        out.present = true;

        // Validator
        if(!fd.validator(in, &fd.rule))
            return false;

        // Sanitizer
        return fd.sanitizer(in, &fd.rule, out.value);
    }

    template <std::size_t... Is> bool Clean(const InputType& input, CleanedType& out, std::index_sequence<Is...>) const
    {
        return (... && VTSField(std::get<Is>(fieldRules), input[Is], std::get<Is>(out)));
    }

private: // Rendering
    template <std::size_t... Is> void RenderFields(std::index_sequence<Is...>)
    {
        // Fold expression to unroll fields
        (RenderOneField<Is>(), ...);
    }

    template <std::size_t I> void RenderOneField()
    {
        const auto& name = fieldNames[I];
        const auto& fd = std::get<I>(fieldRules);

        // Label
        preRenderedFields += "  <label for=\"";
        preRenderedFields += formName;
        preRenderedFields += "__";
        preRenderedFields += name;
        preRenderedFields += "\">";
        preRenderedFields += name;
        preRenderedFields += "</label>\n";

        // Input start
        preRenderedFields += "  <input id=\"";
        preRenderedFields += formName;
        preRenderedFields += "__";
        preRenderedFields += name;
        preRenderedFields += "\" name=\"";
        preRenderedFields += name;
        preRenderedFields += "\" ";

        // Rule attributes
        RenderInputAttributes(preRenderedFields, fd.rule);

        // Close input
        preRenderedFields += "/>\n";
    }

private: // Storage
    std::string_view formName;
    NamesArray fieldNames;
    FieldsTuple fieldRules;
    std::string preRenderedFields;
};

} // namespace WFX::Form

#endif // WFX_INC_FORMS_HPP