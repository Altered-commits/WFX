#include "timer_heap.hpp"

namespace WFX::Utils {

// vvv Main Functions vvv
bool TimerHeap::Insert(std::uint64_t data, std::uint64_t delay, std::uint64_t delta) noexcept
{
    // Does data already exist? If so gg, we don't really want duplicate entries
    auto it = idMap_.find(data);
    if(it != idMap_.end())
        return false;

    // Bucket coalesce
    delay = RoundToBucket(delay, delta);

    std::size_t idx = heap_.size();
    heap_.emplace_back(TimerNode{data, delay, idx});

    // Insert into map, rollback if fails
    idMap_.emplace(data, idx);

    FixHeap(idx);
    return true;
}

bool TimerHeap::Remove(std::uint64_t data) noexcept
{
    auto it = idMap_.find(data);
    if(it == idMap_.end())
        return false;

    std::size_t idx = it->second;
    std::size_t lastIdx = heap_.size() - 1;

    idMap_.erase(data);

    if(idx != lastIdx) {
        heap_[idx] = heap_[lastIdx];
        heap_[idx].heapIdx = idx;

        if(auto it = idMap_.find(heap_[idx].data); it != idMap_.end())
            it->second = idx;
    }

    heap_.pop_back();
    if(idx < heap_.size())
        FixHeap(idx);

    return true;
}

bool TimerHeap::PopExpired(std::uint64_t now, std::uint64_t& outData) noexcept
{
    if(heap_.empty())
        return false;

    TimerNode& min = heap_[0];

    if(min.delay > now)
        return false;

    outData = min.data;
    return Remove(outData);
}

TimerNode* TimerHeap::GetMin() noexcept
{
    return heap_.empty() ? nullptr : &heap_.front();
}

std::size_t TimerHeap::Size() const noexcept
{
    return heap_.size();
}

// vvv Helper Functions vvv
void TimerHeap::FixHeap(std::size_t idx) noexcept
{
    // Try sift-up
    while(idx > 0) {
        std::size_t parent = (idx - 1) / 2;
        if(heap_[idx].delay >= heap_[parent].delay)
            break;
        SwapNodes(heap_[idx], heap_[parent]);
        idx = parent;
    }

    // Then sift-down
    std::size_t n = heap_.size();
    while(true) {
        std::size_t smallest = idx;
        std::size_t l = 2 * idx + 1;
        std::size_t r = 2 * idx + 2;

        if(l < n && heap_[l].delay < heap_[smallest].delay)
            smallest = l;
        if(r < n && heap_[r].delay < heap_[smallest].delay)
            smallest = r;
        if(smallest == idx)
            break;

        SwapNodes(heap_[idx], heap_[smallest]);
        idx = smallest;
    }
}

void TimerHeap::SwapNodes(TimerNode& lhs, TimerNode& rhs) noexcept
{
    std::swap(lhs, rhs);

    lhs.heapIdx = &lhs - &heap_[0];
    rhs.heapIdx = &rhs - &heap_[0];

    if(auto it = idMap_.find(lhs.data); it != idMap_.end())
        it->second = lhs.heapIdx;

    if(auto it = idMap_.find(rhs.data); it != idMap_.end())
        it->second = rhs.heapIdx;
}

std::uint64_t TimerHeap::RoundToBucket(std::uint64_t expire, std::uint64_t delta) noexcept
{
    if(!delta)
        return expire;

    std::uint64_t half = delta >> 1;
    return (expire + half) / delta * delta;
}

} // namespace WFX::Utils