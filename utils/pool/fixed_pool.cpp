// configurable_fixed_allocator_set.cpp
#include "fixed_pool.hpp"

namespace WFX::Utils {

void* PlatformMemory::Allocate(std::size_t size)
{
#if defined(_WIN32)
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return ptr == MAP_FAILED ? nullptr : ptr;
#endif
}

void PlatformMemory::Free(void* ptr, std::size_t size)
{
#if defined(_WIN32)
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

FixedAllocPool::FixedAllocPool(std::size_t blockSize, std::size_t slabSize)
    : blockSize_(blockSize), slabSize_(slabSize)
{
    assert(blockSize_ >= sizeof(FreeNode));
}

FixedAllocPool::~FixedAllocPool()
{
    for(void* slab : slabs_)
        PlatformMemory::Free(slab, slabSize_);
}

void FixedAllocPool::PreWarm(std::size_t blockCount)
{
    std::vector<void*> temp;
    temp.reserve(blockCount);

    for(std::size_t i = 0; i < blockCount; ++i) {
        void* ptr = Allocate();
        if(!ptr) break;
        
        temp.push_back(ptr);
    }

    for(void* ptr : temp)
        Free(ptr);
}

void* FixedAllocPool::Allocate()
{
    FreeNode* node = freeList_.load(std::memory_order_acquire);
    while(node) {
        FreeNode* next = node->next;
        if(freeList_.compare_exchange_weak(node, next, std::memory_order_acq_rel))
            return node;
    }
    return AllocateSlab();
}

void FixedAllocPool::Free(void* ptr)
{
    auto*     node = static_cast<FreeNode*>(ptr);
    FreeNode* head = freeList_.load(std::memory_order_acquire);
    do {
        node->next = head;
    }
    while(!freeList_.compare_exchange_weak(head, node, std::memory_order_release));
}

void* FixedAllocPool::AllocateSlab()
{
    std::lock_guard lock(slabMutex_);
    
    void* slab = PlatformMemory::Allocate(slabSize_);
    if(!slab) return nullptr;

    slabs_.push_back(slab);
    
    char*       base   = static_cast<char*>(slab);
    std::size_t blocks = slabSize_ / blockSize_;

    for(std::size_t i = 1; i < blocks; ++i)
        Free(base + i * blockSize_);

    return base;
}

ConfigurableFixedAllocPool::ConfigurableFixedAllocPool(std::initializer_list<std::size_t> allowedSizes)
{
    assert(!allowedSizes.size() || *allowedSizes.begin() >= 8);

    std::size_t index = 0;

    for(std::size_t s : allowedSizes) {
        assert(Math::IsPowerOfTwo(s));
    
        allocators_.push_back(new FixedAllocPool(s));
    
        int l2 = Math::Log2(s);
        if(l2 < minLog2_) minLog2_ = l2;
        if(l2 > maxLog2_) maxLog2_ = l2;
    
        ++index;
    }

    log2ToIndex_.resize(maxLog2_ - minLog2_ + 1, -1);
    index = 0;

    for(std::size_t s : allowedSizes) {
        int l2 = Math::Log2(s);
        log2ToIndex_[l2 - minLog2_] = static_cast<int8_t>(index++);
    }
}

ConfigurableFixedAllocPool::~ConfigurableFixedAllocPool()
{
    for(FixedAllocPool* a : allocators_)
        delete a;
}

void ConfigurableFixedAllocPool::PreWarmAll(std::size_t blocksPerAllocator)
{
    for(FixedAllocPool* allocator : allocators_)
        allocator->PreWarm(blocksPerAllocator);
}

void* ConfigurableFixedAllocPool::Allocate(std::size_t size)
{
    if(FixedAllocPool* a = FindAllocator(size))
        return a->Allocate();

    return nullptr;
}

void ConfigurableFixedAllocPool::Free(void* ptr, std::size_t size)
{
    FixedAllocPool* a = FindAllocator(size);
    assert(a && "Free called with unsupported size");
    a->Free(ptr);
}

FixedAllocPool* ConfigurableFixedAllocPool::FindAllocator(std::size_t size) const
{
    int log2val = Math::Log2RoundUp(size);
    if(log2val < minLog2_ || log2val > maxLog2_) return nullptr;
    
    int idx = log2ToIndex_[log2val - minLog2_];
    
    return idx >= 0 ? allocators_[idx] : nullptr;
}

} // namespace WFX::Utils