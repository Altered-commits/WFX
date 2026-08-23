// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#include "timer_heap.hpp"
#include "utils/diagnostics/logger.hpp"

namespace WFX::Utils {

TimerHeap::TimerHeap()
{
    idMap_.Init(64);
}

// vvv Main Functions vvv
bool TimerHeap::Insert(std::uint64_t data, std::uint64_t delay, std::uint64_t delta) noexcept
{
    // Does data already exist? If so gg, we don't really want duplicate entries
    if(idMap_.Get(data) != nullptr)
        return false;

    // Bucket coalesce
    delay = RoundToBucket(delay, delta);

    const std::size_t idx = heap_.size();
    heap_.emplace_back(TimerNode{data, delay, idx});

    // idMap_ is Init()'d in the constructor and 'data' was just confirmed absent above, so this
    // can only fail if that invariant is somehow broken, leaving heap_ and idMap_ out of sync
    if(!idMap_.Insert(data, idx))
        GetLogger().Fatal("[TimerHeap]: idMap_ insert unexpectedly failed for a key just confirmed absent");

    FixHeap(idx);
    return true;
}

bool TimerHeap::Remove(std::uint64_t data) noexcept
{
    std::size_t* idxPtr = idMap_.Get(data);
    if(!idxPtr)
        return false;

    const std::size_t idx = *idxPtr;
    const std::size_t lastIdx = heap_.size() - 1;

    idMap_.Erase(data);

    if(idx != lastIdx) {
        heap_[idx] = heap_[lastIdx];
        heap_[idx].heapIdx = idx;

        if(auto* p = idMap_.Get(heap_[idx].data))
            *p = idx;
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

    const TimerNode& min = heap_[0];

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
        const std::size_t parent = (idx - 1) / 2;
        if(heap_[idx].delay >= heap_[parent].delay)
            break;

        SwapNodes(heap_[idx], heap_[parent]);
        idx = parent;
    }

    // Then sift-down
    const std::size_t n = heap_.size();
    while(true) {
        std::size_t smallest = idx;
        const std::size_t l = 2 * idx + 1;
        const std::size_t r = 2 * idx + 2;

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

    if(auto* p = idMap_.Get(lhs.data))
        *p = lhs.heapIdx;

    if(auto* p = idMap_.Get(rhs.data))
        *p = rhs.heapIdx;
}

std::uint64_t TimerHeap::RoundToBucket(std::uint64_t expire, std::uint64_t delta) noexcept
{
    if(!delta)
        return expire;

    const std::uint64_t half = delta >> 1;
    return (expire + half) / delta * delta;
}

} // namespace WFX::Utils