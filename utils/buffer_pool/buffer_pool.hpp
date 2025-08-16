#ifndef WFX_UTILS_BUFFER_POOL_HPP
#define WFX_UTILS_BUFFER_POOL_HPP

#include "utils/logger/logger.hpp"

#include <cstddef>
#include <mutex>
#include <memory>
#include <functional>

namespace WFX::Utils {

// Wrapper around TLSF by Matthew Conte
class BufferPool {
public:
    using ResizeCallback = std::function<std::size_t(std::size_t)>;

    BufferPool(std::uint16_t shardCount, std::size_t initialSize, ResizeCallback resizeCb = nullptr);
    ~BufferPool();

private:
    // Forward declare the private implementation struct for a single shard
    struct Shard;

public:
    void* Lease(std::size_t size);
    void* Reacquire(void* ptr, std::size_t newSize);
    void  Release(void* ptr);

private:
    void* AllocateFromShard(Shard& shard, std::size_t totalSize);

private:
    // Cuz moving / copying pool makes 0 sense
    BufferPool(const BufferPool&)            = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&)                 = delete;
    BufferPool& operator=(BufferPool&&)      = delete;

private:
    Shard& GetShardForThread(std::uint16_t& outShardIndex);

    // Platform-specific memory alignment functions
    void* AlignedMalloc(std::size_t size, std::size_t alignment);
    void  AlignedFree(void* ptr);

private:
    Logger& logger_ = Logger::GetInstance();

    // The vector of memory pool shards. Each thread is assigned a shard
    std::unique_ptr<Shard[]> shards_;
    std::uint16_t            shardCount_;
    std::size_t              initialSize_;
    ResizeCallback           resizeCallback_;
};

} // namespace WFX::Utils

#endif // WFX_UTILS_BUFFER_POOL_HPP