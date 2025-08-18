#ifndef WFX_UTILS_TIMER_WHEEL_HPP
#define WFX_UTILS_TIMER_WHEEL_HPP

#include <cstdint>
#include <vector>
#include <functional>

namespace WFX::Utils {

static constexpr std::uint32_t NIL = 0xFFFFFFFFu;

enum class TimeUnit : std::uint8_t {
    Seconds,
    Milliseconds,
    Microseconds
};

struct SlotMeta {
    std::uint32_t next   = NIL;
    std::uint32_t prev   = NIL;
    std::uint16_t bucket = 0;
    std::uint8_t  rounds = 0;
    std::uint8_t  flags  = 0;
};

class TimerWheel {
public:
    TimerWheel()  = default;
    ~TimerWheel() = default;

public:
    void          Init(std::uint32_t capacity,
                       std::uint32_t wheelSlots,
                       std::uint32_t tickVal,
                       TimeUnit unit = TimeUnit::Milliseconds);
    void          Reinit(std::uint32_t capacity);
    void          SetTick(std::uint32_t val, TimeUnit unit);
    std::uint64_t GetTick();
    void          Schedule(std::uint32_t pos, std::uint64_t timeout);
    void          Cancel(std::uint32_t pos);
    void          Tick(std::uint64_t nowTick, std::function<void(std::uint32_t)> onExpire);

private:
    void          Unlink(std::uint32_t pos);
    void          ClearSlot(std::uint32_t pos);
    std::uint64_t ToTicks(std::uint64_t duration) const;

private:
    std::uint32_t cap_      = 0;
    std::uint32_t slots_    = 0;
    std::uint32_t mask_     = 0;
    std::uint16_t shift_    = 0;
    std::uint16_t tickVal_  = 1;     // tick size in unit
    std::uint64_t nowTick_  = 0;     // current tick counter
    TimeUnit      unit_     = TimeUnit::Milliseconds;

    std::vector<SlotMeta>      meta_;
    std::vector<std::uint32_t> wheelHeads_;
};

static_assert(sizeof(TimerWheel) <= 80, "TimerWheel must be <= 80 bytes.");

} // namespace WFX::Utils

#endif // WFX_UTILS_TIMER_WHEEL_HPP