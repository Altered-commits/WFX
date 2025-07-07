#include "math.hpp"

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace WFX::Utils {

// 2 ^ power functions
std::size_t Math::RoundUpToPowerOfTwo(std::size_t x)
{
    if(x == 0) return 1;

    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
#if SIZE_MAX > UINT32_MAX
    x |= x >> 32;
#endif
    return ++x;
}

bool Math::IsPowerOfTwo(std::size_t x)
{
    return x && !(x & (x - 1));
}

// Log functions
int Math::Log2(std::size_t x)
{
    if(x == 0)
        return -1;

#if defined(_MSC_VER)
    unsigned long index;
    #if defined(_M_X64) || defined(_WIN64)
        _BitScanReverse64(&index, x);
    #else
        _BitScanReverse(&index, static_cast<unsigned long>(x));
    #endif
        return static_cast<int>(index);
#elif defined(__GNUC__) || defined(__clang__)
    if constexpr(sizeof(std::size_t) == 8)
        return 63 - __builtin_clzl(x);
    else
        return 31 - __builtin_clz(static_cast<unsigned int>(x));
#else
    int r = 0;
    while (x >>= 1) ++r;
    return r;
#endif
}

int Math::Log2RoundUp(std::size_t x)
{
    if(x == 0)
        return -1;

    return Math::Log2(x) + !Math::IsPowerOfTwo(x);
}

} // namespace WFX::Utils
