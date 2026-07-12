// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_ABI_STRING_VIEW_HPP
#define WFX_SHARED_ABI_STRING_VIEW_HPP

#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace WFX::Shared {

struct StringView {
public:
    const char* data;
    std::uint64_t length;

public: // vvv Basic methods vvv
    bool Empty() const noexcept
    {
        return length == 0;
    }
    std::uint64_t Size() const noexcept
    {
        return length;
    }
    const char* Data() const noexcept
    {
        return data;
    }
    const char* Begin() const noexcept
    {
        return data;
    }
    const char* End() const noexcept
    {
        return data + length;
    }
    char At(std::uint64_t index) const noexcept
    {
        return data[index];
    }

public: // vvv Comparison vvv
    bool Equals(const StringView& other) const noexcept
    {
        if(length != other.length)
            return false;

        for(std::uint64_t i = 0; i < length; ++i)
            if(data[i] != other.data[i])
                return false;

        return true;
    }

    int Compare(const StringView& other) const noexcept
    {
        const std::uint64_t minLen = (length < other.length) ? length : other.length;

        for(std::uint64_t i = 0; i < minLen; ++i)
            if(data[i] != other.data[i])
                return (data[i] < other.data[i]) ? -1 : 1;

        if(length == other.length)
            return 0;

        return (length < other.length) ? -1 : 1;
    }

public: // vvv Factory vvv
    static StringView FromCString(const char* str) noexcept
    {
        StringView sv{};
        sv.data = str;

        if(!str) {
            sv.length = 0;
            return sv;
        }

        std::uint64_t len = 0;
        while(str[len] != '\0')
            ++len;

        sv.length = len;
        return sv;
    }
};

static_assert(sizeof(StringView) == 16, "'WFX_StringView' ABI size mismatch");
static_assert(alignof(StringView) == alignof(void*), "'WFX_StringView' alignment mismatch");
static_assert(std::is_standard_layout<StringView>::value, "'WFX_StringView' must be standard layout");
static_assert(std::is_trivially_copyable<StringView>::value, "'WFX_StringView' must be trivially copyable");

} // namespace WFX::Shared

#endif // WFX_SHARED_ABI_STRING_VIEW_HPP