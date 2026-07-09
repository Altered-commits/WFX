// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_WFX_MEMORY_HPP
#define WFX_INC_WFX_MEMORY_HPP

// -----------------------------------------------------------------------
// wfx/memory.hpp
// User-facing wrappers over the engine's allocator (shared/apis/memory_api.hpp).
//
// The engine and every user-space allocation should come from the same
// allocator, so prefer these over raw new/delete/malloc in user-space code.
//
// Provides:
//   WFX::Alloc(size)        : raw allocation, like malloc
//   WFX::Realloc(ptr, size) : raw resize, like realloc
//   WFX::Free(ptr)          : raw free
//   WFX::New<T>(args...)    : allocate + construct, like `new T(args...)`
//   WFX::Delete(ptr)        : destroy + free, like `delete ptr`
//   WFX::Allocator<T>       : std::allocator-compatible, stateless. Use with
//                             std::vector<T, WFX::Allocator<T>>,
//                             std::basic_string<char, std::char_traits<char>, WFX::Allocator<char>>, etc.
//   WFX::Vector<T>          : std::vector<T, WFX::Allocator<T>>
//   WFX::String             : std::basic_string<..., WFX::Allocator<char>>
//
//   auto* p = WFX::New<MyType>(arg1, arg2);
//   WFX::Delete(p);
//
//   WFX::Vector<int> v; // backing storage allocated via WFX::Alloc/Free
//   WFX::String s = "hi";
// -----------------------------------------------------------------------

#include "core/core.hpp"
#include <cstddef>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace WFX {

// vvv Base Logic vvv
inline void* Alloc(std::size_t size) noexcept
{
    return Core::MemoryApiExt1()->Alloc(size);
}

inline void* Realloc(void* ptr, std::size_t newSize) noexcept
{
    return Core::MemoryApiExt1()->Realloc(ptr, newSize);
}

inline void Free(void* ptr) noexcept
{
    if(ptr)
        Core::MemoryApiExt1()->Free(ptr);
}

// vvv Object Logic vvv
// Allocates via WFX::Alloc, then placement-constructs T. Pair with WFX::Delete
template <typename T, typename... Args> T* New(Args&&... args)
{
    void* mem = Alloc(sizeof(T));
    if(!mem)
        return nullptr;

    return ::new (mem) T(std::forward<Args>(args)...);
}

// Destroys and frees an object allocated with WFX::New. No-op on null
template <typename T> void Delete(T* ptr) noexcept
{
    if(!ptr)
        return;

    ptr->~T();
    Free(ptr);
}

// vvv Allocator Logic vvv
template <typename T> struct Allocator {
    using value_type = T;

public:
    Allocator() noexcept = default;
    template <typename U> Allocator(const Allocator<U>&) noexcept
    {}

public: // Functions
    T* allocate(std::size_t n)
    {
        void* p = WFX::Alloc(n * sizeof(T));
        if(!p)
            throw std::bad_alloc();

        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t) noexcept
    {
        WFX::Free(p);
    }

public: // Operators
    template <typename U> bool operator==(const Allocator<U>&) const noexcept
    {
        return true;
    }
    template <typename U> bool operator!=(const Allocator<U>&) const noexcept
    {
        return false;
    }
};

// vvv Useful Aliases vvv
template <typename T> using Vector = std::vector<T, Allocator<T>>;
using String = std::basic_string<char, std::char_traits<char>, Allocator<char>>;

} // namespace WFX

#endif // WFX_INC_WFX_MEMORY_HPP
