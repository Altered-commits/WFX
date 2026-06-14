// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_UTILS_TIMER_WHEEL_HPP
#define WFX_UTILS_TIMER_WHEEL_HPP

#include <cstdint>
#include <vector>
#include <functional>

namespace WFX::Utils {

static constexpr std::uint32_t NIL = 0xFFFFFFFFu;

enum class TimeUnit : std::uint8_t { SECONDS, MILLISECONDS, MICROSECONDS };

struct SlotMeta {
    std::uint32_t next = NIL; // |
    std::uint32_t prev = NIL; // |-> 8 bytes

    std::uint32_t extra = 0;       // |
    std::uint16_t bucket = 0xFFFF; // |
    std::uint8_t rounds = 0;       // |-> 8 bytes
};
static_assert(sizeof(SlotMeta) == 16, "SlotMeta must STRICTLY be 16 bytes.");

using OnExpireCallback = std::function<void(std::uint32_t id, std::uint32_t extra)>;

class TimerWheel {
public:
    TimerWheel() = default;
    ~TimerWheel() = default;

public:
    void Init(std::uint32_t capacity, std::uint32_t wheelSlots, std::uint32_t tickVal, TimeUnit unit,
              OnExpireCallback onExpire);
    void Reinit(std::uint32_t capacity);
    void Expand(std::uint32_t extraCapacity);
    void SetTick(std::uint32_t val, TimeUnit unit);
    std::uint64_t GetTick() const noexcept;
    void Schedule(std::uint32_t pos, std::uint32_t extra, std::uint64_t timeout);
    void Cancel(std::uint32_t pos);
    void Tick(std::uint64_t nowTick);

private:
    void Unlink(std::uint32_t pos);
    void ClearSlot(std::uint32_t pos);

private:
    std::uint32_t cap_ = 0;
    std::uint32_t slots_ = 0;
    std::uint32_t mask_ = 0;
    std::uint16_t shift_ = 0;
    std::uint16_t tickVal_ = 1; // Tick size in unit
    std::uint64_t nowTick_ = 0; // Current tick counter
    TimeUnit unit_ = TimeUnit::MILLISECONDS;

    OnExpireCallback onExpire_;
    std::vector<SlotMeta> meta_;
    std::vector<std::uint32_t> wheelHeads_;
};

} // namespace WFX::Utils

#endif // WFX_UTILS_TIMER_WHEEL_HPP