#include "timer_wheel.hpp"
#include "utils/logger/logger.hpp"
#include <cassert>

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace WFX::Utils {

// vvv Helper Function vvv
static inline unsigned CountTrailingZeros(std::uint64_t x) noexcept
{
    if(x == 0u)
        return 64u;

#if defined(_MSC_VER)
        unsigned long index = 0;
    #if defined(_M_X64)
        _BitScanForward64(&index, x);
        return static_cast<unsigned>(index);
    #elif defined(_M_IX86)
        unsigned long low  = static_cast<unsigned long>(x & 0xFFFFFFFFu);
        if(_BitScanForward(&index, low))
            return static_cast<unsigned>(index);
        unsigned long high = static_cast<unsigned long>((x >> 32) & 0xFFFFFFFFu);
        _BitScanForward(&index, high);
        return static_cast<unsigned>(index + 32);
    #else
        // Fallback for unknown MSVC arch
        unsigned n = 0;
        while((x & 1ull) == 0ull) { ++n; x >>= 1; }
        return n;
    #endif
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_ctzll(x));
#else // Unknown architecture
    unsigned n = 0;
    while((x & 1ull) == 0ull) { ++n; x >>= 1; }
    return n;
#endif
}

// vvv User Functions vvv
void TimerWheel::Init(
    std::uint32_t capacity, std::uint32_t wheelSlots, std::uint32_t tickVal,
    TimeUnit unit, OnExpireCallback onExpire
)
{
    auto& logger = Logger::GetInstance();

    if(!onExpire)
        logger.Fatal("[TimerWheel]: 'onExpire' function was nullptr");

    onExpire_ = std::move(onExpire);
    cap_      = capacity;
    slots_    = wheelSlots;
    unit_     = unit;
    tickVal_  = tickVal ? tickVal : 1;

    // Make slots_ a power of 2 (for fast masking)
    if((slots_ & (slots_ - 1)) != 0)
        logger.Fatal("[TimerWheel]: 'wheelSlots' must be a power of two");

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

std::uint64_t TimerWheel::GetTick() const noexcept
{
    return nowTick_;
}

void TimerWheel::Schedule(std::uint32_t pos, std::uint64_t timeout)
{
    // Sanity checks
    assert(pos < cap_ && "TimerWheel.Schedule expected pos < capacity");

    // First cancel if already scheduled
    Unlink(pos);

    // Calculate ticks for this wheel
    std::uint64_t ticks = 0;

    // Check if is a power of two
    if(tickVal_ > 1) {
        if((tickVal_ & (tickVal_ - 1)) == 0) {
            // For power of two, we can use bitwise operations which is far faster than normal div
            unsigned shift = CountTrailingZeros(tickVal_);
            ticks = timeout >> shift;
        }
        // Not a power of two, use div
        else
            ticks = timeout / tickVal_;
    }
    else
        ticks = timeout;

    std::uint64_t expireTick = nowTick_ + ticks;

    std::uint32_t bucket = static_cast<std::uint32_t>(expireTick & mask_);
    std::uint8_t  rounds = static_cast<std::uint8_t>((expireTick >> shift_) - (nowTick_ >> shift_));

    SlotMeta& m = meta_[pos];
    m.bucket    = bucket;
    m.rounds    = rounds;

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

void TimerWheel::Tick(std::uint64_t nowTick)
{
    while(nowTick_ < nowTick) {
        std::uint32_t bucket = static_cast<std::uint32_t>(nowTick_ & mask_);
        std::uint32_t curr   = wheelHeads_[bucket];

        // Process bucket entries
        while(curr != NIL) {
            SlotMeta& m = meta_[curr];
            std::uint32_t next = m.next;

            if(m.rounds == 0) {
                onExpire_(curr);
                Unlink(curr);
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
    if(pos >= cap_)
        return;

    SlotMeta& m = meta_[pos];
    if(m.bucket >= slots_)
        return; // Not linked

    if(m.prev != NIL)
        meta_[m.prev].next = m.next;
    else if(wheelHeads_[m.bucket] == pos)
        wheelHeads_[m.bucket] = m.next;

    if(m.next != NIL)
        meta_[m.next].prev = m.prev;

    // Clear the slot
    m.next = m.prev = NIL;
    m.bucket = slots_;
    m.rounds = 0;
}

void TimerWheel::ClearSlot(std::uint32_t pos)
{
    meta_[pos] = SlotMeta{};
}

} // namespace WFX::Utils