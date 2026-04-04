#ifndef WFX_SHARED_MEMORY_API_HPP
#define WFX_SHARED_MEMORY_API_HPP

#include <cstdint>

namespace WFX::Shared {

enum class MemoryAPIVersion : std::uint8_t {
    V1 = 1,
};

// vvv All aliases for clarity vvv
using AllocFn   = void* (*)(std::uint64_t size);
using ReallocFn = void* (*)(void* ptr, std::uint64_t newSize);
using FreeFn    = void  (*)(void* ptr);

// vvv API declarations vvv
struct MEMORY_API_TABLE {
    AllocFn          Alloc;
    ReallocFn        Realloc;
    FreeFn           Free;

    // Metadata
    MemoryAPIVersion apiVersion;
};

// vvv Getter vvv
const MEMORY_API_TABLE* GetMemoryAPIV1();

} // namespace WFX::Shared

#endif // WFX_SHARED_MEMORY_API_HPP
