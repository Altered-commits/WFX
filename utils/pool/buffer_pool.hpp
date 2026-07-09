// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_BUFFER_POOL_HPP
#define WFX_UTILS_BUFFER_POOL_HPP

#include "utils/diagnostics/logger.hpp"
#include <functional>
#include <vector>

namespace WFX::Utils {

struct BufferShard {
    std::size_t poolSize = 0;
    std::size_t usableSize = 0;
    void* tlsfAllocator = nullptr;
    std::vector<void*> memorySegments;
};

struct BufferPoolStats {
    std::size_t totalAllocations = 0;
    std::size_t totalFrees = 0;
    std::size_t totalReallocs = 0;
    std::size_t poolExpansions = 0;
    std::size_t allocationFailures = 0;
};

// Wrapper around TLSF by Matthew Conte
class BufferPool final {
    using ResizeCallback = std::function<std::size_t(std::size_t)>;
    using OOMCallback = std::function<void(std::size_t, std::size_t, const BufferPoolStats&)>;

public:
    BufferPool() = default;
    ~BufferPool();

public:
    void Init(std::size_t initialSize, ResizeCallback resizeCb = nullptr, OOMCallback oomCb = nullptr);
    bool IsInitialized() const;

public:
    void* Alloc(std::size_t size);
    void* Realloc(void* ptr, std::size_t newSize);
    void Free(void* ptr);
    const BufferPoolStats& GetStats() const;

private:
    void* AllocateFromShard(std::size_t size);
    void* ExpandAndAllocate(std::size_t size);

    void* AlignedMalloc(std::size_t size, std::size_t alignment);
    void AlignedFree(void* ptr);

private:
    Logger& logger_ = GetLogger();

    BufferShard shard_;
    BufferPoolStats stats_;
    ResizeCallback resizeCallback_;
    OOMCallback oomCallback_;
};

// Free function declaration (definition in 'buffer_pool.hpp')
BufferPool& GetBufferPool() noexcept;

} // namespace WFX::Utils

#endif // WFX_UTILS_BUFFER_POOL_HPP