#ifndef WFX_UTILS_TIMER_WHEEL_HPP
#define WFX_UTILS_TIMER_WHEEL_HPP

#include <cstdint>
#include <vector>
#include <functional>

namespace WFX::Utils {

static constexpr std::uint32_t NIL = 0xFFFFFFFFu;

enum class TimeUnit : std::uint8_t {
    SECONDS,
    MILLISECONDS,
    MICROSECONDS
};

// If we are going to store flags inside of 'SlotMeta', might as well predefine some constants-
// -which we will frequently used, not that it makes sense for it to be inside of timer_wheel.hpp
// But yeah
enum TimerFlags : std::uint8_t {
    NONE      = 0,
    TIMEOUT   = 1 << 0,  // Connection timeout at network backend
    SCHEDULER = 1 << 1   // For async timer scheduler (Used in async sleep functions)
};

struct SlotMeta {
    std::uint32_t next      = NIL;
    std::uint32_t prev      = NIL;
    std::uint32_t remainder = 0;
    std::uint16_t bucket    = 0;
    std::uint8_t  rounds    = 0;
    std::uint8_t  flags     = 0;
};

// Just the stuff which are passed in callbacks
struct UserMeta {
    std::uint32_t remainder = 0;
    std::uint8_t  flags     = 0;
};

using OnExpireCallback    = std::function<void(std::uint32_t id, UserMeta meta)>;
using OnMinUpdateCallback = std::function<void(std::uint64_t expireTick)>;

class TimerWheel {
public:
    TimerWheel()  = default;
    ~TimerWheel() = default;

public:
    void          Init(std::uint32_t capacity,
                       std::uint32_t wheelSlots,
                       std::uint32_t tickVal,
                       TimeUnit unit,
                       OnExpireCallback onExpire);
    void          SetMinUpdateCallback(OnMinUpdateCallback onMinUpdate);
    void          Reinit(std::uint32_t capacity);
    void          SetTick(std::uint32_t val, TimeUnit unit);
    std::uint64_t GetTick() const noexcept;
    void          Schedule(std::uint32_t pos, std::uint64_t timeout, std::uint8_t flags);
    void          Cancel(std::uint32_t pos);
    void          Tick(std::uint64_t nowTick);

private:
    void          Unlink(std::uint32_t pos);
    void          ClearSlot(std::uint32_t pos);

private:
    std::uint32_t cap_      = 0;
    std::uint32_t slots_    = 0;
    std::uint32_t mask_     = 0;
    std::uint16_t shift_    = 0;
    std::uint16_t tickVal_  = 1;     // Tick size in unit
    std::uint64_t nowTick_  = 0;     // Current tick counter
    TimeUnit      unit_     = TimeUnit::MILLISECONDS;
    bool          inTick_   = false;

    OnExpireCallback            onExpire_;
    std::vector<SlotMeta>       meta_;
    std::vector<std::uint32_t>  wheelHeads_;

    std::uint64_t               globalMin_ = UINT64_MAX;
    OnMinUpdateCallback         onMinUpdate_;
    std::vector<std::uint64_t>  bucketMin_;
};

} // namespace WFX::Utils

#endif // WFX_UTILS_TIMER_WHEEL_HPP