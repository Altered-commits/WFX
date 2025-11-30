#ifndef WFX_INC_FORM_SANITIZERS_HPP
#define WFX_INC_FORM_SANITIZERS_HPP

#include <string_view>
#include <cstdint>

#include "fields.hpp"
#include "utils/backport/string.hpp"

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
    std::int64_t val = 0;
    if(!WFX::Utils::StrToInt64(sv, val))
        return false;

    return (val >= r.min && val <= r.max);
}

// Strict conversion. This handles both validation and sanitizing
static inline bool DefaultSanitizeUInt(std::string_view sv, const void* fieldPtr, std::uint64_t& out)
{
    const UInt& r = *static_cast<const UInt*>(fieldPtr);

    // All necessary checks are done in 'StrToUInt64'
    std::uint64_t val = 0;
    if(!WFX::Utils::StrToUInt64(sv, val))
        return false;

    return (val >= r.min && val <= r.max);
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

    if(errno != 0)                   return false;
    if(end != sv.data() + sv.size()) return false;

    return (v >= r.min && v <= r.max);
}

// vvv Dispatchers vvv
static inline SanitizerFn<std::string_view> DefaultSanitizerFor(const Text&)  { return DefaultSanitizeText;  }
static inline SanitizerFn<std::string_view> DefaultSanitizerFor(const Email&) { return DefaultSanitizeEmail; }
static inline SanitizerFn<std::int64_t>     DefaultSanitizerFor(const Int&)   { return DefaultSanitizeInt;   }
static inline SanitizerFn<std::uint64_t>    DefaultSanitizerFor(const UInt&)  { return DefaultSanitizeUInt;  }
static inline SanitizerFn<double>           DefaultSanitizerFor(const Float&) { return DefaultSanitizeFloat; }

} // namespace Form

#endif // WFX_INC_FORM_SANITIZERS_HPP