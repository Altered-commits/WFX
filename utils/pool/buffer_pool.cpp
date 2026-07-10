// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "buffer_pool.hpp"

#include <cstdlib>
#include <cstring>
#include <tlsf.h>

/*
 * [02-11-2025]: i'm making buffer pool single sharded, no need for multiple shards, we won't be-
 *               -using threads and stuff
 */
namespace WFX::Utils {

// vvv Constants vvv
static constexpr std::size_t MAX_EXPANSION_ATTEMPTS = 4;

// vvv Main stuff vvv
// Global pool instance
static BufferPool __GlobalBufferPool;

BufferPool& GetBufferPool() noexcept
{
    return __GlobalBufferPool;
}

BufferPool::~BufferPool()
{
    if(shard_.tlsfAllocator)
        tlsf_destroy(shard_.tlsfAllocator);

    for(void* segment : shard_.memorySegments)
        AlignedFree(segment);

    // Need to do this because now every singleton in this system initializes itself-
    // -asap as its all global statics. Meaning it would exist in master process as well
    // And it will print garbage metrics unecessarily
    if(IsInitialized())
        logger_.Info("[BufferPool]: Shutdown successfully. Metrics: ", "allocs=", stats_.totalAllocations, ", ",
                     "frees=", stats_.totalFrees, ", ", "reallocs=", stats_.totalReallocs, ", ",
                     "expansions=", stats_.poolExpansions, ", ", "failures=", stats_.allocationFailures);
}

void BufferPool::Init(std::size_t initialSize, ResizeCallback resizeCb, OOMCallback oomCb)
{
    if(IsInitialized())
        logger_.Fatal("[BufferPool]: Init() called twice");

    if(resizeCb)
        resizeCallback_ = std::move(resizeCb);
    if(oomCb)
        oomCallback_ = std::move(oomCb);

    const std::size_t tlsfOverhead = tlsf_size() + tlsf_pool_overhead();
    const std::size_t rawSize = initialSize + tlsfOverhead;

    void* memory = AlignedMalloc(rawSize, tlsf_align_size());
    if(!memory)
        logger_.Fatal("[BufferPool]: Initial OS allocation failed for ", rawSize, " bytes");

    shard_.tlsfAllocator = tlsf_create_with_pool(memory, rawSize);
    if(!shard_.tlsfAllocator) {
        AlignedFree(memory);
        logger_.Fatal("[BufferPool]: 'tlsf_create_with_pool()' rejected the initial segment");
    }

    shard_.poolSize = rawSize;
    shard_.usableSize = initialSize;
    shard_.memorySegments.push_back(memory);

    logger_.Info("[BufferPool]: Ready, usable=", initialSize, " raw=", rawSize, " (both in bytes)");
}

bool BufferPool::IsInitialized() const
{
    return shard_.tlsfAllocator != nullptr;
}

// vvv Allocators vvv
void* BufferPool::Alloc(std::size_t size)
{
    if(size == 0)
        return nullptr;

    void* ptr = AllocateFromShard(size);

    if(ptr)
        ++stats_.totalAllocations;
    else
        logger_.Error("[BufferPool]: 'Alloc(", size, ")' failed after all expansion attempts");

    return ptr;
}

void* BufferPool::Realloc(void* rawBlock, std::size_t newSize)
{
    // Mirror realloc(3) semantics exactly
    if(!rawBlock)
        return Alloc(newSize);

    if(newSize == 0) {
        Free(rawBlock);
        return nullptr;
    }

    const std::size_t oldSize = tlsf_block_size(rawBlock);

    // Fast path: TLSF extends inplace or coalesces an adjacent free block
    void* newBlock = tlsf_realloc(shard_.tlsfAllocator, rawBlock, newSize);
    if(newBlock) {
        ++stats_.totalReallocs;
        return newBlock;
    }

    // Slow path: 'rawBlock' is still fully valid here, TLSF does not touch it on failure
    newBlock = AllocateFromShard(newSize);
    if(!newBlock) {
        ++stats_.allocationFailures;
        logger_.Error("[BufferPool]: 'Realloc(", newSize, ")' failed, original block preserved at ", rawBlock);
        return nullptr;
    }

    ++stats_.totalAllocations; // The new block is a real allocation that will be freed later

    std::memcpy(newBlock, rawBlock, std::min(oldSize, newSize));
    tlsf_free(shard_.tlsfAllocator, rawBlock);

    ++stats_.totalReallocs;
    ++stats_.totalFrees; // For the old block being freed
    return newBlock;
}

void BufferPool::Free(void* rawBlock)
{
    if(!rawBlock)
        return;

    tlsf_free(shard_.tlsfAllocator, rawBlock);
    ++stats_.totalFrees;
}

const BufferPoolStats& BufferPool::GetStats() const
{
    return stats_;
}

// vvv Helper functions vvv
void* BufferPool::AllocateFromShard(std::size_t size)
{
    void* ptr = tlsf_malloc(shard_.tlsfAllocator, size);
    if(ptr)
        return ptr;

    return ExpandAndAllocate(size);
}

void* BufferPool::ExpandAndAllocate(std::size_t requestedSize)
{
    // TLSF's allocation search (mapping_search in tlsf.c) rounds the request UP to
    // the next second-level size class before searching, so tlsf_malloc(N) actually
    // needs a free block of up to N + N/2^SL_INDEX_COUNT_LOG2 bytes. If we size a
    // fresh segment to exactly the request, that rounded search finds no big-enough
    // block and fails right after a successful expansion (logged as "possible TLSF
    // corruption" -> false OOM). Pad the segment by that worst-case round-up so a
    // single large allocation is actually servable from the new segment.
    // SL_INDEX_COUNT_LOG2 == 5 in TLSF's default (and TLSF_64BIT) configuration.
    constexpr std::size_t TLSF_SL_INDEX_COUNT_LOG2 = 5;
    const std::size_t segmentOverhead = tlsf_pool_overhead() + tlsf_block_size_min();
    const std::size_t sizeClassRoundup = requestedSize >> TLSF_SL_INDEX_COUNT_LOG2;
    const std::size_t minSegment = requestedSize + sizeClassRoundup + segmentOverhead;

    std::size_t idealSize = resizeCallback_ ? resizeCallback_(shard_.poolSize) : shard_.poolSize * 2;

    if(idealSize < minSegment)
        idealSize = minSegment;

    for(std::size_t attempt = 0; attempt < MAX_EXPANSION_ATTEMPTS; ++attempt) {
        // Halve each retry, but never drop below what is needed to serve the request
        const std::size_t trySize = std::max(idealSize >> attempt, minSegment);

        void* newMemory = AlignedMalloc(trySize, tlsf_align_size());
        if(!newMemory) {
            logger_.Warn("[BufferPool]: OS refused ", trySize, " bytes on attempt ", attempt + 1);
            if(trySize == minSegment)
                break;

            continue;
        }

        if(!tlsf_add_pool(shard_.tlsfAllocator, newMemory, trySize)) {
            AlignedFree(newMemory);
            logger_.Warn("[BufferPool]: 'tlsf_add_pool()' rejected segment of ", trySize, " bytes");
            if(trySize == minSegment)
                break;

            continue;
        }

        shard_.poolSize += trySize;
        shard_.usableSize += (trySize - segmentOverhead);
        shard_.memorySegments.push_back(newMemory);
        ++stats_.poolExpansions;

        void* ptr = tlsf_malloc(shard_.tlsfAllocator, requestedSize);
        if(ptr)
            return ptr;

        logger_.Error("[BufferPool]: Allocation failed post-expansion, possible TLSF corruption");
        break;
    }

    ++stats_.allocationFailures;

    if(oomCallback_)
        oomCallback_(requestedSize, shard_.poolSize, stats_);
    else
        logger_.Fatal("[BufferPool]: OOM killer, all ", MAX_EXPANSION_ATTEMPTS, " expansion attempts failed. ",
                      "The pool could not grow to serve ", requestedSize, " bytes. ", "Current pool size is ",
                      shard_.poolSize, " bytes across ", shard_.memorySegments.size(), " segment(s). ",
                      "Set an 'OOMCallback' to handle this gracefully instead of crashing");

    return nullptr;
}

// vvv OS Allocators vvv
void* BufferPool::AlignedMalloc(std::size_t size, std::size_t alignment)
{
    if(alignment < sizeof(void*))
        alignment = sizeof(void*);

    void* ptr = nullptr;
    return (posix_memalign(&ptr, alignment, size) == 0) ? ptr : nullptr;
}

void BufferPool::AlignedFree(void* ptr)
{
    free(ptr);
}

} // namespace WFX::Utils