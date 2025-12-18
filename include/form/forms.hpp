#ifndef WFX_INC_FORMS_HPP
#define WFX_INC_FORMS_HPP

#include "fields.hpp"
#include "validators.hpp"
#include "sanitizers.hpp"
#include "renders.hpp"
#include "http/aliases.hpp"
#include <array>

namespace Form {

// vvv Factory Functions vvv
template<typename Rule>
constexpr auto Field(const char* name, Rule rule)
{
    return std::pair<std::string_view, FieldDesc<Rule>>(
        std::string_view{name},
        FieldDesc<Rule>{
            rule,
            DefaultValidatorFor(rule),
            DefaultSanitizerFor(rule)
        }
    );
}

// vvv Wrapper for sanitized value vvv
template<typename T>
struct CleanedValue {
    T value{};
};

// vvv Tuple Builder vvv
template<typename... Fields>
struct CleanedTupleFor {
    using Type = std::tuple<CleanedValue<typename Fields::RawType>...>;
};

// vvv Error Handling vvv
enum class FormError : std::uint8_t {
    NONE,
    UNSUPPORTED_CONTENT_TYPE,
    MALFORMED,
    CLEAN_FAILED
};

// vvv Main shit vvv
template<typename... Fields>
struct FormSchema {
    /*
     * Fields is std::pair consisting of FieldName + FieldDesc<Rule>, hence the 'Fields::second_type'
     * Created by 'Field' function inside of 'fields.hpp'
     */
public: // Aliases
    static constexpr std::size_t FieldCount = sizeof...(Fields);

    // Stored
    using FieldsTuple = std::tuple<typename Fields::second_type...>;
    using NamesArray  = std::array<std::string_view, FieldCount>;

    // Helper
    using CleanedType = typename CleanedTupleFor<typename Fields::second_type...>::Type;
    using InputType   = std::array<std::string_view, FieldCount>;

public:
    template<std::size_t N>
    constexpr FormSchema(const char (&formName)[N], Fields&&... f)
        : formName{ formName, N - 1 },
          fieldNames{ f.first... },
          fieldRules{ std::move(f.second)... }
    {
        static_assert(N > 1, "FormSchema.formName cannot be empty");

        // Avoid too many reallocs
        preRenderedForm.reserve(512);

        // Pre-render the form into a string so its fast when we need to SSR
        preRenderedForm += "<form class=\"wfx-form-";
        preRenderedForm += this->formName;
        preRenderedForm += "\" data-form=\"";
        preRenderedForm += this->formName;
        preRenderedForm += "\">\n";

        // Render each field
        RenderFields(std::make_index_sequence<FieldCount>{});

        preRenderedForm += "</form>\n";
    }

public: // Main Functions
    // Auto select the parsing type looking at the header
    FormError Parse(Request& req, CleanedType& out) const
    {
        auto [exists, hptr] = req.headers.CheckAndGetHeader("Content-Type");
        if(!exists)
            return FormError::UNSUPPORTED_CONTENT_TYPE;

        // Content-Type can contain multiple fields seperated by ';'
        // What we need is the initial one
        auto ct = WFX::Utils::TrimView(
            (*hptr).substr(0, hptr->find(';'))
        );

        // In memory simple form
        if(WFX::Utils::StringCanonical::InsensitiveStringCompare(
            ct, "application/x-www-form-urlencoded"
        ))
            return ParseStatic(req.body, out);

        // Other types of forms are not supported for now
        return FormError::UNSUPPORTED_CONTENT_TYPE;
    }

    // Parse small, in memory form (like application/x-www-form-urlencoded)
    FormError ParseStatic(std::string_view body, CleanedType& out) const
    {
        InputType input{};
        if(!SplitIntoArray(body, input))
            return FormError::MALFORMED;

        return (
            !Clean(input, out, std::make_index_sequence<FieldCount>{})
                ? FormError::CLEAN_FAILED
                : FormError::NONE
        );
    }

    std::string_view Render() const
    {
        return preRenderedForm;
    }

private: // Helper Functions
    bool SplitIntoArray(std::string_view body, InputType& out) const
    {
        std::size_t fieldIdx = 0;
        std::size_t pos      = 0;

        while(pos <= body.size()) {
            std::size_t start = pos;
            std::size_t end   = body.find('&', pos);
            if(end == std::string_view::npos)
                end = body.size();

            // More pairs than expected
            if(fieldIdx >= FieldCount)
                return false;

            auto kv    = body.substr(start, end - start);
            auto eqPos = kv.find('=');
            // Missing '='
            if(eqPos == std::string_view::npos)
                return false;

            std::string_view key   = kv.substr(0, eqPos);
            std::string_view value = kv.substr(eqPos + 1);

            // Check key matches the schema field at this index
            if(key != fieldNames[fieldIdx])
                return false;

            // Decode value in place
            if(!WFX::Utils::StringCanonical::DecodePercentInplace(value))
                return false;

            out[fieldIdx++] = value;

            pos = end + 1;
        }

        return fieldIdx == FieldCount;
    }

    // Validate Then Sanitize
    template<typename Field>
    bool VTSField(
        const Field& fd,
        std::string_view in,
        CleanedValue<typename Field::RawType>& out
    ) const
    {
        // Validator
        if(!fd.validator(in, &fd.rule))
            return false;

        // Sanitizer
        return fd.sanitizer(in, &fd.rule, out.value);
    }

    template<std::size_t... Is>
    bool Clean(
        const InputType& input,
        CleanedType& out,
        std::index_sequence<Is...>
    ) const
    {
        return (... && VTSField(
            std::get<Is>(fieldRules),
            input[Is],
            std::get<Is>(out)
        ));
    }

private: // Rendering
    template<std::size_t... Is>
    void RenderFields(std::index_sequence<Is...>)
    {
        // Fold expression to unroll fields
        (RenderOneField<Is>(), ...);
    }

    template<std::size_t I>
    void RenderOneField()
    {
        const auto& name = fieldNames[I];
        const auto& fd   = std::get<I>(fieldRules);

        // Label
        preRenderedForm += "  <label for=\"wfx-";
        preRenderedForm += formName;
        preRenderedForm += '-';
        preRenderedForm += name;
        preRenderedForm += "\">";
        preRenderedForm += name;
        preRenderedForm += "</label>\n";

        // Input start
        preRenderedForm += "  <input id=\"wfx-";
        preRenderedForm += formName;
        preRenderedForm += '-';
        preRenderedForm += name;
        preRenderedForm += "\" name=\"";
        preRenderedForm += name;
        preRenderedForm += "\" ";

        // Rule attributes
        RenderInputAttributes(preRenderedForm, fd.rule);

        // Close input
        preRenderedForm += "/>\n";
    }

private: // Storage
    std::string_view formName;
    NamesArray       fieldNames;
    FieldsTuple      fieldRules;
    std::string      preRenderedForm;
};

} // namespace Form

#endif // WFX_INC_FORMS_HPP