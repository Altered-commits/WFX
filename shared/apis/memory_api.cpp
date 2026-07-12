// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "memory_api.hpp"
#include "shared/utils/memory.hpp"

namespace WFX::Shared {

const MemoryAPIExt1* GetMemoryAPIExt1()
{
    // clang-format off
    // NOLINTNEXTLINE(readability-identifier-naming) - singleton table, treated as Global variable
    static const MemoryAPIExt1 GlobalMemoryAPIExt1 = {
        [](std::uint64_t size) { // AllocFn
            return Alloc(size);
        },
        [](void* ptr, std::uint64_t newSize) { // ReallocFn
            return Realloc(ptr, newSize);
        },
        [](void* ptr) { // FreeFn
            Free(ptr);
        }
    };
    // clang-format on

    return &GlobalMemoryAPIExt1;
}

} // namespace WFX::Shared