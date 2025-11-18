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
    bucketMin_.assign(slots_, UINT64_MAX);
}

void TimerWheel::SetMinUpdateCallback(OnMinUpdateCallback onMinUpdate)
{
    onMinUpdate_ = std::move(onMinUpdate);
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

void TimerWheel::Schedule(std::uint32_t pos, std::uint64_t timeout, std::uint8_t flags)
{
    // Sanity checks
    assert(pos < cap_ && "TimerWheel.Schedule expected pos < capacity");

    // First cancel if already scheduled
    Unlink(pos);

    // Calculate ticks for this wheel and remainder for the next
    std::uint64_t ticks     = 0;
    std::uint32_t remainder = 0;

    // Higher level wheel. Check if is a power of two
    if(tickVal_ > 1) {
        if((tickVal_ & (tickVal_ - 1)) == 0) {
            // For power of two, we can use bitwise operations which is far faster than normal div & mod
            unsigned shift = CountTrailingZeros(tickVal_);
            ticks     = timeout >> shift;
            remainder = static_cast<std::uint32_t>(timeout & (tickVal_ - 1));
        }
        // Not a power of two, use div & mod
        else {
            ticks = timeout / tickVal_;
            remainder = static_cast<std::uint32_t>(timeout % tickVal_);
        }
    }
    // Base wheel (tickVal_ is 1 or 0)
    else
        ticks = (tickVal_ == 0) ? 0 : timeout;

    std::uint64_t expireTick = nowTick_ + ticks;

    std::uint32_t bucket = static_cast<std::uint32_t>(expireTick & mask_);
    std::uint8_t  rounds = static_cast<std::uint8_t>((expireTick >> shift_) - (nowTick_ >> shift_));

    // Track earliest deadline for this bucket only if we are scheduling something
    // This won't work for TIMEOUT / NONE flag
    if((flags == TimerFlags::SCHEDULER) && (expireTick < bucketMin_[bucket])) {
        bucketMin_[bucket] = expireTick;

        // Update global if needed
        if(expireTick < globalMin_) {
            globalMin_ = expireTick;
            if(onMinUpdate_)
                onMinUpdate_(expireTick);
        }
    }

    SlotMeta& m = meta_[pos];
    m.bucket    = bucket;
    m.rounds    = rounds;
    m.flags     = flags;
    m.remainder = remainder;

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
    inTick_ = true;
    while(nowTick_ < nowTick) {
        std::uint32_t bucket = static_cast<std::uint32_t>(nowTick_ & mask_);
        std::uint32_t curr   = wheelHeads_[bucket];

        bool bucketHadSchedulerMin = (bucketMin_[bucket] == globalMin_);
        std::uint64_t oldBucketMin = bucketMin_[bucket];

        // Reset bucket min for rescan later if needed
        bool needRecalc = false;

        // Process bucket entries
        while(curr != NIL) {
            SlotMeta& m = meta_[curr];
            std::uint32_t next = m.next;

            if(m.rounds == 0) {
                // Run user side callback
                onExpire_(curr, UserMeta{m.remainder, m.flags});

                // Remove from wheel
                if(m.flags == TimerFlags::SCHEDULER) {
                    // Scheduler timer removal affects bucket min
                    if(!needRecalc)
                        needRecalc = true;
                }

                Unlink(curr);
            }
            else
                --m.rounds;

            curr = next;
        }

        // Recompute bucket minimum if necessary
        if(needRecalc) {
            std::uint64_t newBucketMin = UINT64_MAX;
            curr = wheelHeads_[bucket];

            while(curr != NIL) {
                const auto& x = meta_[curr];
                if(x.flags == TimerFlags::SCHEDULER) {
                    std::uint64_t expireTick = ((std::uint64_t)x.rounds << shift_) | x.bucket;
                    std::uint64_t absRecmp   = expireTick * tickVal_ + x.remainder;
                    if(absRecmp < newBucketMin)
                        newBucketMin = absRecmp;
                }

                curr = x.next;
            }

            bucketMin_[bucket] = newBucketMin;

            // Update global minimum if necessary
            if(bucketHadSchedulerMin || newBucketMin < globalMin_) {
                std::uint64_t g = UINT64_MAX;
                for(std::uint64_t v : bucketMin_)
                    if(v < g) g = v;

                globalMin_ = g;

                if(onMinUpdate_ && g != UINT64_MAX)
                    onMinUpdate_(g);
            }
        }

        ++nowTick_;
    }
    inTick_ = false;
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

    // Only scheduler's participate in min tracking (and if we aren't in function like 'Tick' which does-
    // -recomputing itself)
    if(!inTick_ && m.flags == TimerFlags::SCHEDULER) {
        std::uint32_t bucket = m.bucket;

        if(bucket < slots_) {
            std::uint64_t was = bucketMin_[bucket];

            // Compute this timer's abs value
            std::uint64_t expireTick = ((std::uint64_t)m.rounds << shift_) | m.bucket;
            std::uint64_t absVal     = expireTick * tickVal_ + m.remainder;

            // If this timer WAS the bucket minimum, we must recompute
            if(absVal == was) {
                std::uint64_t newMin = UINT64_MAX;
                std::uint32_t cur    = wheelHeads_[bucket];

                while(cur != NIL) {
                    const auto& x = meta_[cur];

                    if(x.flags == TimerFlags::SCHEDULER) {
                        std::uint64_t expireTick = ((std::uint64_t)x.rounds << shift_) | x.bucket;
                        std::uint64_t absRecmp   = expireTick * tickVal_ + x.remainder;
                        if(absRecmp < newMin)
                            newMin = absRecmp;
                    }

                    cur = x.next;
                }

                bucketMin_[bucket] = newMin;

                // If this bucket held the global minimum, recompute global
                if(was == globalMin_) {
                    std::uint64_t g = UINT64_MAX;
                    for(std::uint64_t v : bucketMin_)
                        if(v < g) g = v;

                    globalMin_ = g;

                    if(onMinUpdate_ && g != UINT64_MAX)
                        onMinUpdate_(g);
                }
            }
        }
    }

    // Clear the slot
    m.next = m.prev = NIL;
    m.bucket    = slots_;
    m.rounds    = 0;
    m.remainder = 0;
    m.flags     = TimerFlags::NONE;
}

void TimerWheel::ClearSlot(std::uint32_t pos)
{
    SlotMeta& m = meta_[pos];
    m = SlotMeta{};
}

} // namespace WFX::Utils