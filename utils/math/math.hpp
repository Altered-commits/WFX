#ifndef WFX_UTILS_MATH_HPP
#define WFX_UTILS_MATH_HPP

#include <cstdint>
#include <cstddef>

namespace WFX::Utils {

// Used as namespace lmao
class Math final {
public:
    // 2 ^ Power functions
    static std::size_t RoundUpToPowerOfTwo(std::size_t x);
    static bool        IsPowerOfTwo(std::size_t x);
    
    // Log functions
    static int Log2(std::size_t x);
    static int Log2RoundUp(std::size_t x);

private:
    Math()  = delete;
    ~Math() = delete;
};

} // namespace WFX::Utils


#endif // WFX_UTILS_MATH_HPP