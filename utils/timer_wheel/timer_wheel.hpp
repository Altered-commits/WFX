#ifndef WFX_UTILS_TIMER_WHEEL_HPP
#define WFX_UTILS_TIMER_WHEEL_HPP

#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>

namespace WFX::Utils {

class TimerWheel {
    static constexpr std::uint32_t NIL = 0xFFFFFFFFu;

public:
    TimerWheel()  = default;
    ~TimerWheel() = default;

public:
    void Init(std::uint32_t capacity, std::uint32_t wheel_slots, std::uint32_t tick_ms);
    void Reinit(std::uint32_t capacity);
    void SetTickMs(std::uint32_t ms) { tick_ms_ = ms; }
    void Schedule(std::uint32_t pos, std::uint32_t timeout_ms);
    void Cancel(std::uint32_t pos);
    void OnMove(std::uint32_t from, std::uint32_t to);
    void OnErase(std::uint32_t pos);
    template<class F> void Tick(std::uint32_t now_tick, F&& onExpire);

private:
    void Unlink(std::uint32_t pos);
    void ClearSlot(std::uint32_t pos);

    std::uint32_t cap_ = 0;
    std::uint32_t slots_ = 4096;
    std::uint32_t mask_ = slots_ - 1;
    std::uint32_t shift_ = 12;
    std::uint32_t tick_ms_ = 1;
    std::uint32_t now_tick_ = 0;

    std::vector<std::uint32_t> tw_next_, tw_prev_;
    std::vector<std::uint16_t> tw_bucket_;
    std::vector<std::uint8_t>  tw_rounds_, tw_flags_;
    std::vector<std::uint32_t> wheel_heads_;
};

} // namespace WFX::Utils

#endif // WFX_UTILS_TIMER_WHEEL_HPP
