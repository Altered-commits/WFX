// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_FORM_FIELDS_HPP
#define WFX_INC_FORM_FIELDS_HPP

#include <tuple>
#include <string_view>
#include <cstdint>

namespace WFX::Form {

// Input: Form data, Form field (type erased)
using ValidatorFn = bool (*)(std::string_view, const void*);

// Input: Form data, Form field (type erased)
// Output: of type T via T&
template <typename T> using SanitizerFn = bool (*)(std::string_view, const void*, T&);

// All common / required rules to exist in every rule
struct BaseRule {
    bool required = true;
};

// Kept as the full representable range (rather than 0) so that sanitizers.hpp's
// unconditional `out >= min && out <= max` check never rejects input when a
// field's bound was never explicitly set.
inline constexpr std::int64_t INT_UNBOUNDED_MIN = INT64_MIN;
inline constexpr std::int64_t INT_UNBOUNDED_MAX = INT64_MAX;
inline constexpr std::uint64_t UINT_UNBOUNDED_MIN = 0;
inline constexpr std::uint64_t UINT_UNBOUNDED_MAX = UINT64_MAX;
inline constexpr double FLOAT_UNBOUNDED_MIN = -1e308;
inline constexpr double FLOAT_UNBOUNDED_MAX = 1e308;

// vvv Builtin Form Rules vvv
struct Text : BaseRule {
    bool ascii = false;
    std::uint32_t min = 0;
    std::uint32_t max = 65535;
};

struct Email : BaseRule {
    bool strict = true;
};

struct Int : BaseRule {
    std::int64_t min = INT_UNBOUNDED_MIN;
    std::int64_t max = INT_UNBOUNDED_MAX;
};

struct UInt : BaseRule {
    std::uint64_t min = UINT_UNBOUNDED_MIN;
    std::uint64_t max = UINT_UNBOUNDED_MAX;
};

struct Float : BaseRule {
    double min = FLOAT_UNBOUNDED_MIN;
    double max = FLOAT_UNBOUNDED_MAX;
};

// vvv Form Type Traits vvv
template <typename Rule> struct DecayedType;

template <> struct DecayedType<Text> {
    using Type = std::string_view;
};
template <> struct DecayedType<Email> {
    using Type = std::string_view;
};
template <> struct DecayedType<Int> {
    using Type = std::int64_t;
};
template <> struct DecayedType<UInt> {
    using Type = std::uint64_t;
};
template <> struct DecayedType<Float> {
    using Type = double;
};

// vvv Field Descriptor vvv
template <typename Rule> struct FieldDesc {
    using RawType = typename DecayedType<Rule>::Type;

    Rule rule{};
    ValidatorFn validator = nullptr;
    SanitizerFn<RawType> sanitizer = nullptr;
};

} // namespace WFX::Form

#endif // WFX_INC_FORM_FIELDS_HPP