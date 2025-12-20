#ifndef WFX_INC_FORM_SANITIZERS_HPP
#define WFX_INC_FORM_SANITIZERS_HPP

#include "fields.hpp"
#include "utils/backport/string.hpp"
#include <cstdint>
#include <string_view>

namespace Form {

// No processing needed, validation handles everything
static inline bool DefaultSanitizeText(std::string_view sv, const void* _, std::string_view& out)
{
    out = sv;
    return true;
}

// Same as text. Real email normalization goes in user's custom sanitizer
static inline bool DefaultSanitizeEmail(std::string_view sv, const void* _, std::string_view& out)
{
    out = sv;
    return true;
}

// Strict conversion. This handles both validation and sanitizing
static inline bool DefaultSanitizeInt(std::string_view sv, const void* fieldPtr, std::int64_t& out)
{
    const Int& r = *static_cast<const Int*>(fieldPtr);

    // All necessary checks are done in 'StrToInt64'
    if(!WFX::Utils::StrToInt64(sv, out))
        return false;

    return (out >= r.min && out <= r.max);
}

// Strict conversion. This handles both validation and sanitizing
static inline bool DefaultSanitizeUInt(std::string_view sv, const void* fieldPtr, std::uint64_t& out)
{
    const UInt& r = *static_cast<const UInt*>(fieldPtr);

    // All necessary checks are done in 'StrToUInt64'
    if(!WFX::Utils::StrToUInt64(sv, out))
        return false;

    return (out >= r.min && out <= r.max);
}

// Strict conversion. This handles both validation and sanitizing
static inline bool DefaultSanitizeFloat(std::string_view sv, const void* fieldPtr, double& out)
{
    const Float& r = *static_cast<const Float*>(fieldPtr);
    if(sv.empty())
        return false;

    char* end = nullptr;
    errno = 0;

    double v = std::strtod(sv.data(), &end);

    if(
        errno != 0
        || end != sv.data() + sv.size()
        || v < r.min
        || v > r.max
    )
        return false;

    out = v;
    return true;
}

// vvv Dispatchers vvv
static constexpr SanitizerFn<std::string_view> DefaultSanitizerFor(const Text&)  { return DefaultSanitizeText;  }
static constexpr SanitizerFn<std::string_view> DefaultSanitizerFor(const Email&) { return DefaultSanitizeEmail; }
static constexpr SanitizerFn<std::int64_t>     DefaultSanitizerFor(const Int&)   { return DefaultSanitizeInt;   }
static constexpr SanitizerFn<std::uint64_t>    DefaultSanitizerFor(const UInt&)  { return DefaultSanitizeUInt;  }
static constexpr SanitizerFn<double>           DefaultSanitizerFor(const Float&) { return DefaultSanitizeFloat; }

} // namespace Form

#endif // WFX_INC_FORM_SANITIZERS_HPP