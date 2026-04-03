#ifndef WFX_SHARED_ABI_VARIANT_HPP
#define WFX_SHARED_ABI_VARIANT_HPP

#include "string_view.hpp"
#include "uuid.hpp"

namespace WFX::Shared {

// Common tags
enum : std::uint8_t {
    VARIANT_EMPTY  = 0,
    VARIANT_U64    = 1,
    VARIANT_I64    = 2,
    VARIANT_STR    = 3,
    VARIANT_UUID   = 4,
};

struct alignas(8) Variant {
public:
    std::uint8_t tag;
    std::uint8_t _pad[7];

    union {
        std::uint64_t u64;
        std::int64_t  i64;
        StringView    str;
        UUID          uuid;
    } data;

public: // vvv Basic methods vvv
    void         Reset()          noexcept { tag = VARIANT_EMPTY; }
    bool         HasValue() const noexcept { return tag != VARIANT_EMPTY; }
    std::uint8_t Tag()      const noexcept { return tag; }

public: // vvv Accessors vvv
    std::uint64_t AsU64()    const noexcept { return data.u64; }
    std::int64_t  AsI64()    const noexcept { return data.i64; }
    StringView    AsString() const noexcept { return data.str; }
    UUID          AsUUID()   const noexcept { return data.uuid; }

public: // vvv Factory vvv
    static Variant Empty() noexcept
    {
        Variant v{};
        v.tag = VARIANT_EMPTY;
        return v;
    }

    static Variant FromU64(std::uint64_t v64) noexcept
    {
        Variant v{};
        v.tag = VARIANT_U64;
        v.data.u64 = v64;
        return v;
    }

    static Variant FromI64(std::int64_t v64) noexcept
    {
        Variant v{};
        v.tag = VARIANT_I64;
        v.data.i64 = v64;
        return v;
    }

    static Variant FromString(StringView sv) noexcept
    {
        Variant v{};
        v.tag = VARIANT_STR;
        v.data.str = sv;
        return v;
    }

    static Variant FromUUID(const UUID& id) noexcept
    {
        Variant v{};
        v.tag = VARIANT_UUID;
        v.data.uuid = id;
        return v;
    }
};

static_assert(alignof(Variant) == 8,                      "WFX_Variant alignment mismatch");
static_assert(sizeof(Variant) >= 24,                      "WFX_Variant too small");
static_assert(std::is_standard_layout<Variant>::value,    "WFX_Variant must be standard layout");
static_assert(std::is_trivially_copyable<Variant>::value, "WFX_Variant must be trivially copyable");

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_VARIANT_HPP