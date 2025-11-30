#ifndef WFX_INC_FORM_FIELDS_HPP
#define WFX_INC_FORM_FIELDS_HPP

#include <tuple>
#include <string_view>
#include <cstdint>

namespace Form {

// Input: Form data, Form field (type erased)
using ValidatorFn = bool (*)(std::string_view, const void*);

// Input: Form data, Form field (type erased)
// Output: of type T via T&
template<typename T>
using SanitizerFn = bool (*)(std::string_view, const void*, T&);

// vvv Builtin Form Fields vvv
struct Text {
    std::uint32_t min             = 0;
    std::uint32_t max             = 65535;
    bool          ascii           = false;
    ValidatorFn   customValidator = nullptr;
};

struct Email  {
    bool        strict          = true;
    ValidatorFn customValidator = nullptr;
};

struct Int {
    std::int64_t min             = INT64_MIN;
    std::int64_t max             = INT64_MAX;
    ValidatorFn  customValidator = nullptr;
};

struct UInt {
    std::uint64_t min             = 0;
    std::uint64_t max             = UINT64_MAX;
    ValidatorFn   customValidator = nullptr;
};

struct Float {
    double      min             = -1e308;
    double      max             = 1e308;
    ValidatorFn customValidator = nullptr;
};

// vvv Form Type Traits vvv
template<typename Rule>
struct DecayedType;

template<> struct DecayedType<Text>  { using type = std::string_view; };
template<> struct DecayedType<Email> { using type = std::string_view; };
template<> struct DecayedType<Int>   { using type = std::int64_t;     };
template<> struct DecayedType<UInt>  { using type = std::uint64_t;    };
template<> struct DecayedType<Float> { using type = double;           };

// vvv Field Descriptor vvv
template<typename Rule>
struct FieldDesc {
    using RawType = typename DecayedType<Rule>::type;

    std::string_view     name;
    Rule                 rule{};
    SanitizerFn<RawType> sanitizer = nullptr;

    constexpr FieldDesc CustomSanitize(SanitizerFn<RawType> fn) const
    {
        FieldDesc copy = *this;
        copy.sanitizer = fn;
        return copy;
    }

    constexpr FieldDesc Validator(ValidatorFn fn) const
    {
        FieldDesc copy = *this;
        copy.rule.customValidator = fn;
        return copy;
    }
};

// vvv Factory Functions vvv
template<typename Rule>
constexpr auto Field(const char* name, Rule rule)
{
    return FieldDesc<Rule>{ name, rule, nullptr };
}

template<typename... Fields>
struct FieldList { std::tuple<Fields...> fields; };

template<typename... Fields>
constexpr auto MakeFields(Fields... f)
{
    return FieldList<Fields...>{ std::make_tuple(f...) };
}

} // namespace Form

#endif // WFX_INC_FORM_FIELDS_HPP