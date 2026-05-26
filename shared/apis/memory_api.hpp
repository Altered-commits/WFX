#ifndef WFX_SHARED_MEMORY_API_HPP
#define WFX_SHARED_MEMORY_API_HPP

#include <cstdint>
#include <type_traits>

namespace WFX::Shared {

// vvv All aliases for clarity vvv
using AllocFn = void* (*)(std::uint64_t size);
using ReallocFn = void* (*)(void* ptr, std::uint64_t newSize);
using FreeFn = void (*)(void* ptr);

// vvv API declarations vvv
struct MEMORY_API_EXT1 {
    AllocFn Alloc;
    ReallocFn Realloc;
    FreeFn Free;
};
static_assert(std::is_standard_layout<MEMORY_API_EXT1>::value, "'MEMORY_API_EXT1' must be standard layout");

// vvv Getter vvv
const MEMORY_API_EXT1* GetMemoryAPIExt1();

} // namespace WFX::Shared

#endif // WFX_SHARED_MEMORY_API_HPP
