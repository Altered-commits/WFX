#ifndef WFX_UTILS_TIMER_HEAP_HPP
#define WFX_UTILS_TIMER_HEAP_HPP

#include <vector>
#include <unordered_map>
#include <cstdint>
#include "utils/hash_map/hash_shard.hpp"

namespace WFX::Utils {

struct TimerNode {
    std::uint64_t data;
    std::uint64_t delay;     // Expiration time
    std::size_t   heapIdx;   // Index in the heap
};

class TimerHeap {
public:
    TimerHeap(BufferPool&);
    ~TimerHeap() = default;

public: // Main Functions
    bool        Insert(std::uint64_t data, std::uint64_t delay, std::uint64_t delta) noexcept;
    bool        Remove(std::uint64_t data)                                           noexcept;
    bool        PopExpired(std::uint64_t now, std::uint64_t& outData)                noexcept;
    TimerNode*  GetMin()                                                             noexcept;
    std::size_t Size()                                                         const noexcept;

private: // Helper Functions
    void          FixHeap(std::size_t idx)                                 noexcept;
    void          SwapNodes(TimerNode& lhs, TimerNode& rhs)                noexcept;
    std::uint64_t RoundToBucket(std::uint64_t expire, std::uint64_t delta) noexcept;

private:
    std::vector<TimerNode> heap_;
    HashShard<std::uint64_t, std::size_t> idMap_;
};

} // namespace WFX::Utils

#endif // WFX_UTILS_TIMER_HEAP_HPP