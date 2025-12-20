#ifndef WFX_INC_FORM_VALIDATORS_HPP
#define WFX_INC_FORM_VALIDATORS_HPP

#include "fields.hpp"
#include "utils/backport/string.hpp"
#include <cstdlib>

namespace Form {

static inline bool DefaultValidateText(std::string_view sv, const void* fieldPtr)
{
    const Text& r = *static_cast<const Text*>(fieldPtr);

    std::size_t n = sv.size();
    if(n < r.min || n > r.max)
        return false;

    if(r.ascii) {
        for(unsigned char c : sv)
            if(c > 127) return false;
    }

    return true;
}

static inline bool DefaultValidateEmail(std::string_view sv, const void* fieldPtr)
{
    const Email& r = *static_cast<const Email*>(fieldPtr);

    if(sv.empty())
        return false;

    // ASCII-only check
    if(r.strict) {
        for(unsigned char c : sv)
            if(c > 127) return false;
    }

    // Find '@'
    std::size_t atPos = sv.find('@');
    if(atPos == std::string_view::npos || atPos == 0 || atPos == sv.size() - 1)
        return false;

    // Ensure only one '@'
    if(sv.find('@', atPos + 1) != std::string_view::npos)
        return false;

    std::string_view local  = sv.substr(0, atPos);
    std::string_view domain = sv.substr(atPos + 1);

    // Local part checks
    if(local.empty() || local.front() == '.' || local.back() == '.')
        return false;
    if(local.find("..") != std::string_view::npos)
        return false;

    for(char c : local)
        if(!(isalnum(c) || c == '_' || c == '.' || c == '-'))
            return false;

    // Domain part checks
    if(domain.empty() || domain.front() == '.' || domain.back() == '.')
        return false;
    if(domain.find("..") != std::string_view::npos)
        return false;

    for(char c : domain)
        if(!(isalnum(c) || c == '-' || c == '.'))
            return false;

    // Domain must contain at least one dot
    if(domain.find('.') == std::string_view::npos)
        return false;

    return true;
}

static inline bool DefaultValidateInt(std::string_view sv, const void* fieldPtr)
{
    /*
     * NOTE: Sanitizers will handle both validation and output, this just needs to return true
     */
    return true;
}

static inline bool DefaultValidateUInt(std::string_view sv, const void* fieldPtr)
{
    /*
     * NOTE: Sanitizers will handle both validation and output, this just needs to return true
     */
    return true;
}

static inline bool DefaultValidateFloat(std::string_view sv, const void* fieldPtr)
{
    /*
     * NOTE: Sanitizers will handle both validation and output, this just needs to return true
     */
    return true;
}

// vvv Dispatchers vvv
static constexpr ValidatorFn DefaultValidatorFor(const Text&)  { return DefaultValidateText;  }
static constexpr ValidatorFn DefaultValidatorFor(const Email&) { return DefaultValidateEmail; }
static constexpr ValidatorFn DefaultValidatorFor(const Int&)   { return DefaultValidateInt;   }
static constexpr ValidatorFn DefaultValidatorFor(const UInt&)  { return DefaultValidateUInt;  }
static constexpr ValidatorFn DefaultValidatorFor(const Float&) { return DefaultValidateFloat; }

} // namespace Form

#endif // WFX_INC_FORM_VALIDATORS_HPP