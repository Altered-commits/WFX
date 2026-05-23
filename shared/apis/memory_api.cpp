#include "memory_api.hpp"
#include "utils/pool/buffer_pool.hpp"

namespace WFX::Shared {

const MEMORY_API_EXT1* GetMemoryAPIExt1()
{
    static MEMORY_API_EXT1 __GlobalAsyncAPIExt1 = {
        [](std::uint64_t size) { // AllocFn
            return Utils::GetBufferPool().Alloc(size);
        },
        [](void* ptr, std::uint64_t newSize) { // ReallocFn
            return Utils::GetBufferPool().Realloc(ptr, newSize);
        },
        [](void* ptr) { // FreeFn
            Utils::GetBufferPool().Free(ptr);
        }
    };

    return &__GlobalAsyncAPIExt1;
}

} // namespace WFX::Shared