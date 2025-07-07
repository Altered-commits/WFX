#ifndef WFX_UTILS_CONFIGURABLE_FIXED_ALLOCATOR_SET_HPP
#define WFX_UTILS_CONFIGURABLE_FIXED_ALLOCATOR_SET_HPP

#include "utils/math/math.hpp"

#include <vector>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace WFX::Utils {

class PlatformMemory
{
public:
    static void* Allocate(std::size_t size);
    static void  Free(void* ptr, std::size_t size);
};

class FixedAllocPool
{
public:
    explicit FixedAllocPool(std::size_t blockSize, std::size_t slabSize = 1 << 20);
    ~FixedAllocPool();

    void        PreWarm(std::size_t blockCount);
    void*       Allocate();
    void        Free(void* ptr);
    std::size_t BlockSize() const { return blockSize_; }

private:
    void* AllocateSlab();

private:
    std::size_t blockSize_;
    std::size_t slabSize_;

    struct FreeNode {
        FreeNode* next;
    };

    std::atomic<FreeNode*> freeList_{nullptr};
    std::vector<void*>     slabs_;
    std::mutex             slabMutex_;
};

class ConfigurableFixedAllocPool
{
public:
    explicit ConfigurableFixedAllocPool(std::initializer_list<std::size_t> allowedSizes);
    ~ConfigurableFixedAllocPool();

    void  PreWarmAll(std::size_t blocksPerAllocator);
    void* Allocate(std::size_t size);
    void  Free(void* ptr, std::size_t size);

private:
    FixedAllocPool* FindAllocator(std::size_t size) const;

private:
    std::vector<FixedAllocPool*> allocators_;
    std::vector<int8_t> log2ToIndex_;
    
    int minLog2_ = 64;
    int maxLog2_ = 0;
};

} // namespace WFX::Utils

#endif // WFX_UTILS_CONFIGURABLE_FIXED_ALLOCATOR_SET_HPP