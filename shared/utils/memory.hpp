// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_UTILS_MEMORY_HPP
#define WFX_SHARED_UTILS_MEMORY_HPP

#include <cstddef>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef WFX_ENGINE_BUILD
#include "utils/pool/buffer_pool.hpp"
#else
#include "core/core.hpp"
#endif

namespace WFX::Shared {

// vvv Base Logic vvv
#ifdef WFX_ENGINE_BUILD
inline void* Alloc(std::size_t size) noexcept
{
    return Utils::GetBufferPool().Alloc(size);
}

inline void* Realloc(void* ptr, std::size_t newSize) noexcept
{
    return Utils::GetBufferPool().Realloc(ptr, newSize);
}

inline void Free(void* ptr) noexcept
{
    if(ptr)
        Utils::GetBufferPool().Free(ptr);
}
#else
inline void* Alloc(std::size_t size) noexcept
{
    return Core::MemoryApiExt1()->alloc(size);
}

inline void* Realloc(void* ptr, std::size_t newSize) noexcept
{
    return Core::MemoryApiExt1()->realloc(ptr, newSize);
}

inline void Free(void* ptr) noexcept
{
    if(ptr)
        Core::MemoryApiExt1()->free(ptr);
}
#endif

// vvv Object Logic vvv
// Allocates via Alloc, then placement-constructs T. Pair with Delete
template <typename T, typename... Args> T* New(Args&&... args)
{
    void* mem = Alloc(sizeof(T));
    if(!mem)
        return nullptr;

    return ::new (mem) T(std::forward<Args>(args)...);
}

// Destroys and frees an object allocated with New. No-op on null
template <typename T> void Delete(T* ptr) noexcept
{
    if(!ptr)
        return;

    ptr->~T();
    Free(ptr);
}

// vvv Array Logic vvv
// Allocates via Alloc, then value-constructs 'count' elements of T. Pair with DeleteArray
template <typename T> T* NewArray(std::size_t count)
{
    T* mem = static_cast<T*>(Alloc(count * sizeof(T)));
    if(!mem)
        return nullptr;

    for(std::size_t i = 0; i < count; ++i)
        ::new (mem + i) T();

    return mem;
}

// Destroys 'count' elements and frees an array allocated with NewArray. No-op on null
template <typename T> void DeleteArray(T* ptr, std::size_t count) noexcept
{
    if(!ptr)
        return;

    for(std::size_t i = count; i-- > 0;)
        ptr[i].~T();

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
    // Name is dictated by the standard's Allocator named requirement -> std::allocator_traits-
    // -and every container that takes this as a template arg call it by this exact name
    // NOLINTNEXTLINE(readability-identifier-naming)
    T* allocate(std::size_t n)
    {
        void* p = Alloc(n * sizeof(T));
        if(!p)
            throw std::bad_alloc();

        return static_cast<T*>(p);
    }

    // Same as allocate() above, name fixed by the Allocator named requirement
    // NOLINTNEXTLINE(readability-identifier-naming)
    void deallocate(T* p, std::size_t) noexcept
    {
        Free(p);
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
// Owning containers, pool-allocated
// Sequence
template <typename T> using Vector = std::vector<T, Allocator<T>>;
template <typename T> using Deque = std::deque<T, Allocator<T>>;
using String = std::basic_string<char, std::char_traits<char>, Allocator<char>>;

// Hashed
template <typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K>>
using UnorderedMap = std::unordered_map<K, V, H, E, Allocator<std::pair<const K, V>>>;
template <typename T, typename H = std::hash<T>, typename E = std::equal_to<T>>
using UnorderedSet = std::unordered_set<T, H, E, Allocator<T>>;

// Ordered
template <typename K, typename V, typename C = std::less<K>>
using Map = std::map<K, V, C, Allocator<std::pair<const K, V>>>;
template <typename T, typename C = std::less<T>>
using Set = std::set<T, C, Allocator<T>>;

} // namespace WFX::Shared

#endif // WFX_SHARED_UTILS_MEMORY_HPP
