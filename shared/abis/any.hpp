// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_ABI_ANY_HPP
#define WFX_SHARED_ABI_ANY_HPP

#include <cstdint>
#include <type_traits>

namespace WFX::Shared {

struct Any {
public:
    void* data;
    void (*destructor)(void*);
    std::uint64_t typeID;

public:
    void Reset() noexcept
    {
        if(data && destructor)
            destructor(data);

        data = nullptr;
        destructor = nullptr;
        typeID = 0;
    }

    bool HasValue() const noexcept
    {
        return data != nullptr;
    }
    void* Get() noexcept
    {
        return data;
    }
    const void* Get() const noexcept
    {
        return data;
    }
    std::uint64_t TypeID() const noexcept
    {
        return typeID;
    }

    template <typename T> T* As() noexcept
    {
        if(typeID != TypeIDOf<T>())
            return nullptr;

        return static_cast<T*>(data);
    }

    template <typename T> const T* As() const noexcept
    {
        if(typeID != TypeIDOf<T>())
            return nullptr;

        return static_cast<const T*>(data);
    }

public: // vvv Factory vvv
    template <typename T> static std::uint64_t TypeIDOf() noexcept
    {
        static const std::uint8_t tag = 0;
        return reinterpret_cast<std::uint64_t>(&tag);
    }
};

static_assert(sizeof(Any) == 24, "'WFX_Any' ABI must be 24 bytes");
static_assert(alignof(Any) == alignof(std::uint64_t), "'WFX_Any' alignment mismatch");
static_assert(std::is_standard_layout<Any>::value, "'WFX_Any' must be standard layout");
static_assert(std::is_trivially_copyable<Any>::value, "'WFX_Any' must be trivially copyable");

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_ANY_HPP