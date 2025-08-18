#include "timer_wheel.hpp"
#include <cassert>

namespace WFX::Utils {

// vvv User Functions vvv
void TimerWheel::Init(std::uint32_t capacity,
                      std::uint32_t wheelSlots,
                      std::uint32_t tick_val,
                      TimeUnit unit) {
    cap_     = capacity;
    slots_   = wheelSlots;
    unit_    = unit;
    tickVal_ = tick_val ? tick_val : 1;

    // Make slots_ a power of 2 (for fast masking)
    assert((slots_ & (slots_ - 1)) == 0 && "TimerWheel.wheelSlots must be a power of two");
    mask_  = slots_ - 1;
    shift_ = 0;
    while((1u << shift_) < slots_) ++shift_;

    nowTick_ = 0;

    meta_.assign(cap_, SlotMeta{});
    wheelHeads_.assign(slots_, NIL);
}

void TimerWheel::Reinit(std::uint32_t capacity)
{
    cap_ = capacity;
    meta_.assign(cap_, SlotMeta{});
}

void TimerWheel::SetTick(std::uint32_t val, TimeUnit unit)
{
    tickVal_ = val ? val : 1;
    unit_    = unit;
}

std::uint64_t TimerWheel::GetTick()
{
    return nowTick_;
}

void TimerWheel::Schedule(std::uint32_t pos, std::uint64_t timeout)
{
    // Sanity checks
    assert(pos < cap_ && "TimerWheel.Schedule expected pos < capacity");

    // First cancel if already scheduled
    Unlink(pos);

    std::uint64_t ticks       = ToTicks(timeout);
    std::uint64_t expire_tick = nowTick_ + ticks;

    std::uint32_t bucket = static_cast<std::uint32_t>(expire_tick & mask_);
    std::uint8_t  rounds = static_cast<std::uint8_t>(expire_tick >> shift_);

    SlotMeta& m = meta_[pos];
    m.bucket = bucket;
    m.rounds = rounds;

    // Insert at head of wheelHeads_[bucket]
    m.next = wheelHeads_[bucket];
    m.prev = NIL;
    if(m.next != NIL)
        meta_[m.next].prev = pos;

    wheelHeads_[bucket] = pos;
}

void TimerWheel::Cancel(std::uint32_t pos)
{
    // Sanity Checks
    assert(pos < cap_);
    Unlink(pos);
    ClearSlot(pos);
}

void TimerWheel::Tick(std::uint64_t nowTick, std::function<void(std::uint32_t)> onExpire)
{
    while(nowTick_ < nowTick) {
        std::uint32_t bucket = static_cast<std::uint32_t>(nowTick_ & mask_);
        std::uint32_t head   = wheelHeads_[bucket];

        std::uint32_t curr = head;
        while(curr != NIL) {
            SlotMeta& m = meta_[curr];
            std::uint32_t next = m.next;

            if(m.rounds == 0) {
                // Expire this timer
                Unlink(curr);
                onExpire(curr);
            }
            else
                --m.rounds;

            curr = next;
        }

        ++nowTick_;
    }
}

// vvv Helper Functions vvv
void TimerWheel::Unlink(std::uint32_t pos)
{
    if(pos >= cap_) return;

    SlotMeta& m = meta_[pos];
    if(m.bucket >= slots_) return; // Not linked

    if(m.prev != NIL)
        meta_[m.prev].next = m.next;
    else if (wheelHeads_[m.bucket] == pos)
        wheelHeads_[m.bucket] = m.next;

    if(m.next != NIL)
        meta_[m.next].prev = m.prev;

    m.next = m.prev = NIL;
    m.bucket = 0;
    m.rounds = 0;
}

void TimerWheel::ClearSlot(std::uint32_t pos)
{
    SlotMeta& m = meta_[pos];
    m = SlotMeta{};
}

std::uint64_t TimerWheel::ToTicks(std::uint64_t duration) const
{
    // Convert given duration in `unit_` to ticks based on tickVal_
    if(tickVal_ == 0)
        return 0;
    return duration / tickVal_;
}

} // namespace WFX::Utils