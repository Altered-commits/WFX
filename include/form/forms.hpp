#ifndef WFX_INC_FORMS_HPP
#define WFX_INC_FORMS_HPP

#include <tuple>
#include <type_traits>
#include <utility>
#include "fields.hpp"
#include "validators.hpp"
#include "sanitizers.hpp"

namespace Form {

// vvv Wrapper for sanitized value vvv
template<typename T>
struct CleanedValue {
    T value{};
};

// vvv Main shit (using CRTP) vvv
template<typename Derived>
struct FormSchema {
    // Validate a single field
    template<typename Rule>
    static bool ValidateField(const FieldDesc<Rule>& fd, std::string_view sv)
    {
        ValidatorFn v = fd.rule.customValidator ? fd.rule.customValidator : DefaultValidatorFor(fd.rule);
        return v(sv, &fd.rule);
    }

    // Sanitize a single field
    template<typename FieldDescT>
    static bool SanitizeField(const FieldDescT& fd, std::string_view sv,
                              CleanedValue<typename FieldDescT::RawType>& out)
    {
        using CleanT = typename FieldDescT::RawType;

        auto fn = fd.sanitizer ? fd.sanitizer : DefaultSanitizerFor(fd.rule);
        return fn(sv, &fd.rule, out.value);
    }

    // Parse entire form tuple and return cleaned struct
    template<typename... Fields, std::size_t... Is>
    static auto ParseTuple(const std::tuple<Fields...>& fields,
                        const std::tuple<std::string_view...>& input,
                        std::index_sequence<Is...>)
    {
        // Construct a tuple of CleanedValues directly, call SanitizeField inline
        return std::make_tuple(
            ([](const auto& fd, std::string_view sv) {
                CleanedValue<typename std::decay_t<decltype(fd)>::RawType> out{};
                SanitizeField(fd, sv, out);
                return out;
            })(std::get<Is>(fields), std::get<Is>(input))...
        );
    }

    template<typename... Fields>
    static auto Parse(const std::tuple<Fields...>& fields, const std::tuple<std::string_view...>& input)
    {
        return ParseTuple(fields, input, std::index_sequence_for<Fields...>{});
    }
};

} // namespace Form

#endif // WFX_INC_FORMS_HPP