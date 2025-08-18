#include "buffer_pool.hpp"

#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <thread>
#include <functional> // For std::hash

#if defined(_WIN32)
    #include <malloc.h>
#endif

extern "C" {
    #include <tlsf.h>
}

namespace WFX::Utils {

// This small header will be placed immediately before the memory block
struct AllocationHeader {
    std::uint16_t shardIndex;
};

// Each shard is a self-contained memory pool with its own lock
struct alignas(64) BufferPool::Shard {
    std::mutex           mutex;
    std::size_t          poolSize      = 0;
    tlsf_t               tlsfAllocator = nullptr;
    std::vector<void*>   memorySegments;
};

BufferPool::BufferPool(std::uint16_t shardCount, std::size_t initialSize, ResizeCallback resizeCb)
    : resizeCallback_(resizeCb), shardCount_(shardCount)
{
    // Shard count must be a power of two
    if(((shardCount_) & (shardCount_ - 1)))
        logger_.Fatal("[BufferPool]: Shard count must be a power of 2, got: ", shardCount);

    shards_ = std::make_unique<Shard[]>(shardCount);
    if(!shards_)
        logger_.Fatal("[BufferPool]: Failed to allocate memory for pool shards.");

    // Calculate initial size per shard. Ensure a reasonable minimum size
    std::size_t sizePerShard = initialSize / shardCount_;
    if(sizePerShard < 64 * 1024)
        sizePerShard = 64 * 1024;

    for(unsigned int i = 0; i < shardCount_; ++i) {
        void* memory = AlignedMalloc(sizePerShard, tlsf_align_size());
        if(!memory)
            logger_.Fatal("[BufferPool]: Initial malloc failed for shard ", i);

        shards_[i].tlsfAllocator = tlsf_create_with_pool(memory, sizePerShard);
        if(!shards_[i].tlsfAllocator)
            logger_.Fatal("[BufferPool]: Failed to initialize TLSF for shard ", i);

        shards_[i].poolSize = sizePerShard;
        shards_[i].memorySegments.push_back(memory);
    }
    logger_.Info("[BufferPool]: Created ", shardCount_, " shards, each with initial size ", sizePerShard, " bytes.");
}

BufferPool::~BufferPool()
{
    logger_.Info("[BufferPool]: Destroying ", shardCount_, shardCount_ > 1 ? " shards." : " shard.");
    for(unsigned int i = 0; i < shardCount_; ++i) {
        std::lock_guard<std::mutex> lock(shards_[i].mutex);

        if(shards_[i].tlsfAllocator)
            tlsf_destroy(shards_[i].tlsfAllocator);

        for(void* segment : shards_[i].memorySegments)
            AlignedFree(segment);
    }
    logger_.Info("[BufferPool]: Cleanup complete.");
}

// This function is needed for allocation
BufferPool::Shard& BufferPool::GetShardForThread(std::uint16_t& outShardIndex)
{
    static thread_local std::size_t threadIdHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    outShardIndex = static_cast<std::uint16_t>(threadIdHash & (shardCount_ - 1));
    return shards_[outShardIndex];
}

void* BufferPool::Lease(std::size_t size)
{
    std::uint16_t shardIndex;
    Shard& shard = GetShardForThread(shardIndex);
    std::lock_guard<std::mutex> lock(shard.mutex);

    // Allocate enough memory to store AllocationHeader as well
    const std::size_t totalSize = sizeof(AllocationHeader) + size;
    void* rawBlock = AllocateFromShard(shard, totalSize);

    // Place the shard index in the header at the beginning of the block
    AllocationHeader* header = static_cast<AllocationHeader*>(rawBlock);
    header->shardIndex = shardIndex;

    return static_cast<void*>(reinterpret_cast<char*>(rawBlock) + sizeof(AllocationHeader));
}

void* BufferPool::Reacquire(void* ptr, std::size_t newSize)
{
    if(!ptr) return nullptr;

    void* rawBlock = static_cast<char*>(ptr) - sizeof(AllocationHeader);
    AllocationHeader* header = static_cast<AllocationHeader*>(rawBlock);
    std::uint16_t shardIndex = header->shardIndex;

    if(shardIndex >= shardCount_) {
        logger_.Error("[BufferPool]: Corruption detected! Invalid shard index (", shardIndex, ") found in [Re]allocation header.");
        return nullptr;
    }

    Shard& shard = shards_[shardIndex];
    std::lock_guard<std::mutex> lock(shard.mutex);

    const std::size_t totalSize = sizeof(AllocationHeader) + newSize;
    
    // Pass the real TLSF pointer, not the shifted one
    void* newRawBlock = tlsf_realloc(shard.tlsfAllocator, rawBlock, totalSize);

    if(!newRawBlock) {
        // allocate manually
        newRawBlock = AllocateFromShard(shard, totalSize);

        // copy only min(oldSize, totalSize) bytes
        std::size_t copySize = std::min(tlsf_block_size(rawBlock), totalSize);
        std::memcpy(newRawBlock, rawBlock, copySize);

        tlsf_free(shard.tlsfAllocator, rawBlock);
    }

    // update header
    header = static_cast<AllocationHeader*>(newRawBlock);
    header->shardIndex = shardIndex;

    return static_cast<char*>(newRawBlock) + sizeof(AllocationHeader);
}

void BufferPool::Release(void* ptr)
{
    if(!ptr) return;

    // Calculate the address of the original block by moving the pointer backwards
    void* rawBlock = static_cast<void*>(reinterpret_cast<char*>(ptr) - sizeof(AllocationHeader));
    
    // Read the shard index from the header
    AllocationHeader* header = static_cast<AllocationHeader*>(rawBlock);
    std::uint16_t shardIndex = header->shardIndex;

    if(shardIndex >= shardCount_) {
        logger_.Error("[BufferPool]: Corruption detected! Invalid shard index (", shardIndex, ") found in allocation header.");
        return;
    }

    // Get the correct shard and lock it
    Shard& shard = shards_[shardIndex];
    std::lock_guard<std::mutex> lock(shard.mutex);
    
    // Free the entire original block
    tlsf_free(shard.tlsfAllocator, rawBlock);
}

// vvv Helper functions vvv
void* BufferPool::AllocateFromShard(Shard& shard, std::size_t totalSize)
{
    void* rawBlock = tlsf_malloc(shard.tlsfAllocator, totalSize);
    if(rawBlock) return rawBlock;

    // Allocation failed: expand
    std::size_t newSegmentSize = resizeCallback_
        ? resizeCallback_(shard.poolSize)
        : shard.poolSize * 2;
    shard.poolSize += newSegmentSize;

    void* newMemory = AlignedMalloc(newSegmentSize, tlsf_align_size());
    if(!newMemory)
        logger_.Fatal("[BufferPool]: Failed to allocate new memory segment for shard.");

    if(!tlsf_add_pool(shard.tlsfAllocator, newMemory, newSegmentSize))
        logger_.Fatal("[BufferPool]: Failed to add new memory pool to TLSF shard.");

    shard.memorySegments.push_back(newMemory);
    logger_.Debug("[BufferPool]: Added new memory segment of size ", newSegmentSize, " to a shard.");

    rawBlock = tlsf_malloc(shard.tlsfAllocator, totalSize);
    if(!rawBlock)
        logger_.Fatal("[BufferPool]: Allocation failed even after adding a new pool segment.");

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