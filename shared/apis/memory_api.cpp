#include "memory_api.hpp"
#include "utils/pool/buffer_pool.hpp"

namespace WFX::Shared {

using WFX::Utils::Logger;
using WFX::Utils::BufferPool;

const MEMORY_API_TABLE* GetMemoryAPIV1()
{
    static MEMORY_API_TABLE __GlobalAsyncAPIV1 = {
        [](std::uint64_t size) { // AllocFn
            return BufferPool::GetInstance().Alloc(size);
        },
        [](void* ptr, std::uint64_t newSize) { // ReallocFn
            return BufferPool::GetInstance().Realloc(ptr, newSize);
        },
        [](void* ptr) { // FreeFn
            BufferPool::GetInstance().Free(ptr);
        },

        // Version
        MemoryAPIVersion::V1
    };

    return &__GlobalAsyncAPIV1;
}

} // namespace WFX::Shared