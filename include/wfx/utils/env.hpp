// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_UTILS_ENV_HPP
#define WFX_INC_WFX_UTILS_ENV_HPP

// -----------------------------------------------------------------------
// wfx/utils/env.hpp
// Thin typed wrappers over std::getenv for the obviously-common cases.
// Anything more specific (floats, enums, lists, validation) is left to the-
// -caller, plain std::getenv still works directly for that
//
// Provides:
//   WFX::GetEnvString(name, defaultValue = {})
//   WFX::GetEnvBool(name, defaultValue)
//   WFX::GetEnvInt(name, defaultValue)
//
// All three fall back to defaultValue on an unset variable AND on a value-
// -that doesn't parse for the requested type, they don't distinguish the two
// -----------------------------------------------------------------------

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace WFX {

inline std::string_view GetEnvString(const char* name, std::string_view defaultValue = {}) noexcept
{
    const char* value = std::getenv(name);
    return value ? std::string_view(value) : defaultValue;
}

// Accepts "1"/"0" and "true"/"false" (case-insensitive). Anything else, including unset, returns defaultValue
inline bool GetEnvBool(const char* name, bool defaultValue) noexcept
{
    const char* value = std::getenv(name);
    if(!value)
        return defaultValue;

    if(std::strcmp(value, "1") == 0)
        return true;
    if(std::strcmp(value, "0") == 0)
        return false;

    auto ciEquals = [](const char* a, const char* b) noexcept {
        while(*a && *b) {
            if(std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
                return false;
            ++a;
            ++b;
        }
        return *a == '\0' && *b == '\0';
    };

    if(ciEquals(value, "true"))
        return true;
    if(ciEquals(value, "false"))
        return false;

    return defaultValue;
}

// Whole-string integer parse. Trailing garbage after the number, or an unset/empty variable, returns defaultValue
inline std::int64_t GetEnvInt(const char* name, std::int64_t defaultValue) noexcept
{
    const char* value = std::getenv(name);
    if(!value || *value == '\0')
        return defaultValue;

    std::int64_t result = 0;
    const auto len = std::strlen(value);
    const auto [ptr, ec] = std::from_chars(value, value + len, result);

    if(ec != std::errc{} || ptr != value + len)
        return defaultValue;

    return result;
}

} // namespace WFX

#endif // WFX_INC_WFX_UTILS_ENV_HPP
