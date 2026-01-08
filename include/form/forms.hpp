#ifndef WFX_INC_FORMS_HPP
#define WFX_INC_FORMS_HPP

#include "fields.hpp"
#include "validators.hpp"
#include "sanitizers.hpp"
#include "renders.hpp"
#include "http/aliases.hpp"
#include "third_party/json/json.hpp"
#include <array>

// The magic key is purely for SSR json use
// Do not use this key anywhere else
#define FORM_SCHEMA_JSON_KEY "__\x01"

// For ease of use :)
using Json = nlohmann::json;

namespace Form {

// vvv Field Builders vvv
template<typename Rule>
class FieldBuilder {
public: // Helper Aliases
    using DescType = FieldDesc<Rule>;
    using PairType = std::pair<std::string_view, DescType>;

public: // Constructor
    constexpr FieldBuilder(const char* name, Rule rule)
        : pair_{
            std::string_view{name},
            FieldDesc<Rule>{
                rule,
                DefaultValidatorFor(rule),
                DefaultSanitizerFor(rule)
            }
        }
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
    constexpr std::string_view GetName() const & { return pair_.first; }
    constexpr DescType&& GetDesc()            && { return std::move(pair_.second); }

private: // Storage
    PairType pair_;
};

template<typename Rule>
constexpr auto Field(const char* name, Rule rule)
{
    return FieldBuilder{name, std::move(rule)};
}

// vvv Wrapper for sanitized value vvv
template<typename T>
struct CleanedValue {
    T    value{};
    bool present = false;
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
     * Fields is 'FieldBuilder' returned by 'Field' function
     */
public: // Aliases
    static constexpr std::size_t FieldCount = sizeof...(Fields);

    // Stored
    using FieldsTuple = std::tuple<typename Fields::DescType...>;
    using NamesArray  = std::array<std::string_view, FieldCount>;

    // Helper
    using CleanedType = typename CleanedTupleFor<typename Fields::DescType...>::Type;
    using InputType   = std::array<std::string_view, FieldCount>;

public:
    template<std::size_t N>
    constexpr FormSchema(const char (&formName)[N], Fields&&... f)
        : formName{ formName, N - 1 },
        fieldNames{ f.GetName()... },
        fieldRules{ std::move(f).GetDesc()... }
    {
        static_assert(N > 1, "FormSchema.formName cannot be empty");

        // Avoid too many reallocs
        preRenderedFields.reserve(FieldCount * 100);

        // Render each field
        RenderFields(std::make_index_sequence<FieldCount>{});
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

    // Returns view to pre-rendered fields. NOTE: <form></form> needs to be written by user
    std::string_view Render() const
    {
        return preRenderedFields;
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
        // Presence check FIRST
        if(in.empty()) {
            if(fd.rule.required)
                return false;   // Missing required field

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
    NamesArray       fieldNames;
    FieldsTuple      fieldRules;
    std::string      preRenderedFields;
};

// vvv Function for wrapping form as a pointer (so json doesn't copy fields during construction) vvv
// Totally optional, use only for SSR context
template<typename... Fields>
inline Json FormToJson(const FormSchema<Fields...>& form)
{
    static_assert(!std::is_rvalue_reference_v<decltype(form)>,
                  "FormToJson: 'form' cannot be a rvalue, it must strictly be lvalue");

    auto renderWrapper = [](const void* formPtr) -> std::string_view {
        return static_cast<const FormSchema<Fields...>*>(formPtr)->Render();
    };

    return {
        FORM_SCHEMA_JSON_KEY,
        reinterpret_cast<std::uintptr_t>(+renderWrapper),
        reinterpret_cast<std::uintptr_t>(&form)
    };
}

// NOTE: 'nullptr' check is assumed to be done before passing in 'Json*'
inline std::string_view JsonToFormRender(const Json* json)
{
    // Invalid or not SSR form
    if((json->size() != 3) || ((*json)[0].get<std::string>() != FORM_SCHEMA_JSON_KEY))
        return {};

    // Extract pointers
    auto fnPtr   = reinterpret_cast<std::string_view(*)(const void*)>((*json)[1].get<std::uintptr_t>());
    auto formPtr = reinterpret_cast<const void*>((*json)[2].get<std::uintptr_t>());

    if(!fnPtr || !formPtr)
        return {};

    // Call type-erased render
    return fnPtr(formPtr);
}

} // namespace Form

#endif // WFX_INC_FORMS_HPP