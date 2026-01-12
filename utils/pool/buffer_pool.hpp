#ifndef WFX_UTILS_BUFFER_POOL_HPP
#define WFX_UTILS_BUFFER_POOL_HPP

#include "utils/logger/logger.hpp"
#include <functional>

namespace WFX::Utils {

struct BufferShard {
    std::size_t        poolSize      = 0;
    void*              tlsfAllocator = nullptr;
    std::vector<void*> memorySegments;
};

// Wrapper around TLSF by Matthew Conte
class BufferPool final {
    using ResizeCallback = std::function<std::size_t(std::size_t)>;

public:
    static BufferPool& GetInstance();
    void Init(std::size_t initialSize, ResizeCallback resizeCb = nullptr);
    bool IsInitialized();

public:
    void* Lease(std::size_t size);
    void* Reacquire(void* ptr, std::size_t newSize);
    void  Release(void* ptr);

private:
    BufferPool() = default;
    ~BufferPool();

    // No need for copy / move semantics
    BufferPool(const BufferPool&)            = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&)                 = delete;
    BufferPool& operator=(BufferPool&&)      = delete;

private:
    void* AllocateFromShard(std::size_t totalSize);

    // Platform-specific memory alignment functions
    void* AlignedMalloc(std::size_t size, std::size_t alignment);
    void  AlignedFree(void* ptr);

private:
    Logger& logger_ = Logger::GetInstance();

    BufferShard    shard_;
    std::size_t    initialSize_;
    ResizeCallback resizeCallback_;
};

} // namespace WFX::Utils

#endif // WFX_UTILS_BUFFER_POOL_HPP