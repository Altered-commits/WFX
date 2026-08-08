// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_ABI_SEGMENT_VARIANT_HPP
#define WFX_SHARED_ABI_SEGMENT_VARIANT_HPP

#include "string_view.hpp"
#include "uuid.hpp"

namespace WFX::Shared {

// Common tags
enum : std::uint8_t {
    SEG_VARIANT_EMPTY = 0,
    SEG_VARIANT_U64 = 1,
    SEG_VARIANT_I64 = 2,
    SEG_VARIANT_STR = 3,
    SEG_VARIANT_UUID = 4,
    SEG_VARIANT_STC_STR = 5, // Differentiating b/w dynamic and static segment
};

struct alignas(8) SegmentVariant {
public:
    std::uint8_t tag;
    std::uint8_t pad[7];

    union {
        std::uint64_t u64;
        std::int64_t i64;
        StringView str;
        UUID uuid;
    } data;

public: // vvv Basic methods vvv
    void Reset() noexcept
    {
        tag = SEG_VARIANT_EMPTY;
    }
    bool HasValue() const noexcept
    {
        return tag != SEG_VARIANT_EMPTY;
    }
    std::uint8_t Tag() const noexcept
    {
        return tag;
    }

public: // vvv Accessors vvv
    std::uint64_t AsU64() const noexcept
    {
        return data.u64;
    }
    std::int64_t AsI64() const noexcept
    {
        return data.i64;
    }
    StringView AsString() const noexcept
    {
        return data.str;
    }
    UUID AsUUID() const noexcept
    {
        return data.uuid;
    }

public: // vvv Factory vvv
    static SegmentVariant Empty() noexcept
    {
        SegmentVariant v{};
        v.tag = SEG_VARIANT_EMPTY;
        return v;
    }

    static SegmentVariant FromU64(std::uint64_t v64) noexcept
    {
        SegmentVariant v{};
        v.tag = SEG_VARIANT_U64;
        v.data.u64 = v64;
        return v;
    }

    static SegmentVariant FromI64(std::int64_t v64) noexcept
    {
        SegmentVariant v{};
        v.tag = SEG_VARIANT_I64;
        v.data.i64 = v64;
        return v;
    }

    static SegmentVariant FromString(StringView sv, bool isStatic = false) noexcept
    {
        SegmentVariant v{};
        v.tag = isStatic ? SEG_VARIANT_STC_STR : SEG_VARIANT_STR;
        v.data.str = sv;
        return v;
    }

    static SegmentVariant FromUUID(const UUID& id) noexcept
    {
        SegmentVariant v{};
        v.tag = SEG_VARIANT_UUID;
        v.data.uuid = id;
        return v;
    }
};

static_assert(alignof(SegmentVariant) == 8, "'WFX_Variant' alignment mismatch");
static_assert(sizeof(SegmentVariant) == 24, "'WFX_Variant' must be 24 bytes");
static_assert(std::is_standard_layout<SegmentVariant>::value, "'WFX_Variant' must be standard layout");
static_assert(std::is_trivially_copyable<SegmentVariant>::value, "'WFX_Variant' must be trivially copyable");

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_SEGMENT_VARIANT_HPP