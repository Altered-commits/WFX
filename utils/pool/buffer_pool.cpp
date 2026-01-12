#include "buffer_pool.hpp"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
    #include <malloc.h>
#endif

#include <tlsf.h>

namespace WFX::Utils {

/* 
 * [02-11-2025]: i'm making buffer pool single sharded, no need for multiple shards, we won't be-
 *               -using threads and stuff
 */

BufferPool& BufferPool::GetInstance()
{
    static BufferPool pool;
    return pool;
}

BufferPool::~BufferPool()
{
    if(shard_.tlsfAllocator)
        tlsf_destroy(shard_.tlsfAllocator);

    for(void* segment : shard_.memorySegments)
        AlignedFree(segment);

    logger_.Info("[BufferPool]: Cleanup complete");
}

void BufferPool::Init(std::size_t initialSize, ResizeCallback resizeCb)
{
    if(resizeCb)
        resizeCallback_ = std::move(resizeCb);

    // Allocate memory for the pool
    void* memory = AlignedMalloc(initialSize, tlsf_align_size());
    if(!memory)
        logger_.Fatal("[BufferPool]: Initial malloc failed for constructing pool");

    shard_.tlsfAllocator = tlsf_create_with_pool(memory, initialSize);
    if(!shard_.tlsfAllocator)
        logger_.Fatal("[BufferPool]: Failed to initialize TLSF for pool");

    shard_.poolSize = initialSize;
    shard_.memorySegments.push_back(memory);

    logger_.Info("[BufferPool]: Created initial pool of size: ", initialSize, " bytes");
}

bool BufferPool::IsInitialized()
{
    return !shard_.memorySegments.empty();
}

// vvv Allocators vvv
void* BufferPool::Lease(std::size_t size)
{
    // Guaranteed to be not nullptr, hopefully
    return AllocateFromShard(size);
}

void* BufferPool::Reacquire(void* rawBlock, std::size_t newSize)
{
    if(!rawBlock)
        return nullptr;
    
    // Pass the real TLSF pointer, not the shifted one
    void* newRawBlock = tlsf_realloc(shard_.tlsfAllocator, rawBlock, newSize);

    if(!newRawBlock) {
        // Allocate manually
        newRawBlock = AllocateFromShard(newSize);

        // Copy only min(oldSize, newSize) bytes
        std::size_t copySize = std::min(tlsf_block_size(rawBlock), newSize);
        std::memcpy(newRawBlock, rawBlock, copySize);

        tlsf_free(shard_.tlsfAllocator, rawBlock);
    }

    return newRawBlock;
}

void BufferPool::Release(void* rawBlock)
{
    if(!rawBlock)
        return;

    // Free the entire original block
    tlsf_free(shard_.tlsfAllocator, rawBlock);
}

// vvv Helper functions vvv
void* BufferPool::AllocateFromShard(std::size_t totalSize)
{
    void* rawBlock = tlsf_malloc(shard_.tlsfAllocator, totalSize);
    if(rawBlock)
        return rawBlock;

    // Allocation failed: expand
    std::size_t newSegmentSize = resizeCallback_
        ? resizeCallback_(shard_.poolSize) // User callback
        : shard_.poolSize * 2;             // Fallback

    // Sanity check: ensure the new segment can fit the request
    if(newSegmentSize < totalSize)
        newSegmentSize = totalSize * 2;

    shard_.poolSize += newSegmentSize;

    void* newMemory = AlignedMalloc(newSegmentSize, tlsf_align_size());
    if(!newMemory)
        logger_.Fatal("[BufferPool]: Out of memory, failed to allocate new segment of size: ", newSegmentSize, " bytes");

    if(!tlsf_add_pool(shard_.tlsfAllocator, newMemory, newSegmentSize))
        logger_.Fatal("[BufferPool]: TLSF rejected new pool segment (possible corruption)");

    shard_.memorySegments.push_back(newMemory);

    rawBlock = tlsf_malloc(shard_.tlsfAllocator, totalSize);
    if(!rawBlock)
        logger_.Fatal("[BufferPool]: Allocation failed even after expanding pool. Possible TLSF corruption");

    return rawBlock;
}

// AlignedMalloc and AlignedFree remain the same.
void* BufferPool::AlignedMalloc(std::size_t size, std::size_t alignment)
{
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    void* ptr = nullptr;
    if(posix_memalign(&ptr, alignment, size) != 0) return nullptr;
    return ptr;
#endif
}

void BufferPool::AlignedFree(void* ptr)
{
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

} // namespace WFX::Utils