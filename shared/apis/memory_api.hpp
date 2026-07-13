// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_SHARED_MEMORY_API_HPP
#define WFX_SHARED_MEMORY_API_HPP

#include <cstdint>
#include <type_traits>

namespace WFX::Shared {

// vvv All aliases for clarity vvv
using AllocFn = void* (*)(std::uint64_t size);
using ReallocFn = void* (*)(void* ptr, std::uint64_t newSize);
using FreeFn = void (*)(void* ptr);

// vvv API declarations vvv
struct MemoryAPIExt1 {
    AllocFn alloc;
    ReallocFn realloc;
    FreeFn free;
};
static_assert(std::is_standard_layout<MemoryAPIExt1>::value, "'MEMORY_API_EXT1' must be standard layout");

// vvv Getter vvv
const MemoryAPIExt1* GetMemoryAPIExt1();

} // namespace WFX::Shared

#endif // WFX_SHARED_MEMORY_API_HPP
