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

// All common / required rules to exist in every rule
struct BaseRule {
    bool required = true;
};

// vvv Builtin Form Rules vvv
struct Text : BaseRule {
    bool          ascii = false;
    std::uint32_t min   = 0;
    std::uint32_t max   = 65535;
};

struct Email : BaseRule {
    bool strict = true;
};

struct Int : BaseRule {
    std::int64_t min = INT64_MIN;
    std::int64_t max = INT64_MAX;
};

struct UInt : BaseRule {
    std::uint64_t min = 0;
    std::uint64_t max = UINT64_MAX;
};

struct Float : BaseRule {
    double min = -1e308;
    double max = 1e308;
};

// vvv Form Type Traits vvv
template<typename Rule>
struct DecayedType;

template<> struct DecayedType<Text>  { using Type = std::string_view; };
template<> struct DecayedType<Email> { using Type = std::string_view; };
template<> struct DecayedType<Int>   { using Type = std::int64_t;     };
template<> struct DecayedType<UInt>  { using Type = std::uint64_t;    };
template<> struct DecayedType<Float> { using Type = double;           };

// vvv Field Descriptor vvv
template<typename Rule>
struct FieldDesc {
    using RawType = typename DecayedType<Rule>::Type;

    Rule                 rule{};
    ValidatorFn          validator = nullptr;
    SanitizerFn<RawType> sanitizer = nullptr;

    [[nodiscard]]
    constexpr FieldDesc CustomSanitizer(SanitizerFn<RawType> fn) const & noexcept
    {
        FieldDesc copy = *this;
        copy.sanitizer = fn;
        return copy;
    }

    [[nodiscard]]
    constexpr FieldDesc CustomSanitizer(SanitizerFn<RawType> fn) && noexcept
    {
        sanitizer = fn;
        return std::move(*this);
    }

    [[nodiscard]]
    constexpr FieldDesc CustomValidator(ValidatorFn fn) const & noexcept
    {
        FieldDesc copy = *this;
        copy.validator = fn;
        return copy;
    }

    [[nodiscard]]
    constexpr FieldDesc CustomValidator(ValidatorFn fn) && noexcept
    {
        validator = fn;
        return std::move(*this);
    }
};

} // namespace Form

#endif // WFX_INC_FORM_FIELDS_HPP