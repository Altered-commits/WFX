#ifndef WFX_UTILS_EXT_TIMER_WHEEL_HPP
#define WFX_UTILS_EXT_TIMER_WHEEL_HPP

/*
 * TimerWheel but i extended it to contain heirarchical wheels cuz why not
 * This makes my life easier and it supports both timeouts and async timer schedule thingy
 * So yeah
 */

#include "timer_wheel.hpp"

namespace WFX::Utils {

// Two level hierarchical wheel (Ms and Sec)
class ExtendedTimerWheel {
public:
    void Init(std::uint32_t capacity, OnExpireCallback onExpire) noexcept
    {
        // Logic is simple, wheel0_ expires, wheel1_ cascades expiring job to wheel0_ IF remainder exists
        // wheel0_ works on millisecond level precision (1ms ticks)
        // wheel1_ works on second level precision (1024ms ticks)
        wheel1_.Init(capacity, 1024, 1024, TimeUnit::MILLISECONDS,
                    [
                        this,
                        onExpire
                    ](std::uint32_t id, UserMeta meta) {
                        // Remainder is 0 so we expire it now
                        if(meta.remainder == 0) {
                            meta.flags = TimerFlags::NONE;
                            onExpire(id, meta);
                        }
                        // Remainder is > 0 so we cascade it to wheel0_
                        else
                            wheel0_.Schedule(id, meta.remainder, meta.flags);
                    });
        wheel0_.Init(capacity, 1024, 1, TimeUnit::MILLISECONDS, std::move(onExpire));
    }

    void Schedule(std::uint32_t id, std::uint64_t timeoutMs, std::uint8_t flags = TimerFlags::NONE) noexcept
    {
        if(timeoutMs < 1024)
            wheel0_.Schedule(id, timeoutMs, flags);
        else
            wheel1_.Schedule(id, timeoutMs, flags);
    }

    void SetMinUpdateCallback(OnMinUpdateCallback cb)
    {
        minCb_ = std::move(cb);

        wheel0_.SetMinUpdateCallback([this](std::uint64_t minTick) {
            UpdateUnifiedMin(minTick, false);
        });
        wheel1_.SetMinUpdateCallback([this](std::uint64_t minTick) {
            // convert 1024ms tick to ms
            std::uint64_t absMs = minTick << wheel0Shift_;
            UpdateUnifiedMin(absMs, true);
        });
    }

    void Tick(std::uint64_t nowMs) noexcept
    {
        // oldTick0_ = current ms tick of wheel0
        std::uint64_t oldMs = wheel0_.GetTick();

        // Convert to wheel1 ticks (1024ms granularity)
        std::uint64_t oldW1 = oldMs >> wheel0Shift_;
        std::uint64_t newW1 = nowMs >> wheel0Shift_;

        // Cascade ONLY if we crossed 1024ms boundary
        if(newW1 > oldW1)
            wheel1_.Tick(newW1);

        // Now tick wheel0 normally with absolute ms tick
        wheel0_.Tick(nowMs);
    }

    void Cancel(std::uint32_t id) noexcept
    {
        wheel0_.Cancel(id);
        wheel1_.Cancel(id);
    }

    void UpdateUnifiedMin(std::uint64_t newMinMs, bool fromWheel1)
    {
        // Local minima
        if(fromWheel1)
            lastMin1_ = newMinMs;
        else
            lastMin0_ = newMinMs;

        std::uint64_t unified = (lastMin0_ < lastMin1_) ? lastMin0_ : lastMin1_;

        if(minCb_ && unified != UINT64_MAX)
            minCb_(unified);
    }

    std::uint64_t GetTick() const noexcept
    {
        // Why return wheel0_ tick? 
        // The lower wheel (wheel0_) is our main timer ticking clock thingy (ms precision)
        // The upper one (wheel1_) just flows into it whenever it overflows. So returning-
        // -wheel0_.GetTick() basically gives us the current time in ticks that the rest-
        // -of the system cares about
        return wheel0_.GetTick();
    }

private:
    TimerWheel wheel0_, wheel1_;
    OnMinUpdateCallback minCb_;

    std::uint64_t lastMin0_ = UINT64_MAX; // ms
    std::uint64_t lastMin1_ = UINT64_MAX; // ms after conversion

    constexpr static std::uint32_t wheel0Slots_ = 1024;
    constexpr static std::uint32_t wheel0Shift_ = 10; // log2(1024)
    // Range of wheel0 in ms (1024 * 1ms)
    constexpr static std::uint64_t wheel0RangeMs_ = wheel0Slots_ * 1; 
};

} // namespace WFX::Utils

#endif // WFX_UTILS_EXT_TIMER_WHEEL_HPP