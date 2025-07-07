#include "tick_scheduler.hpp"
#include "utils/logger/logger.hpp"
#include <chrono>

namespace WFX::Http {

// TickScheduler::~TickScheduler()
// {
    // I assume that the Stop function will be called properly
    // Without any issue
    // Hopefully
    // Idk
    // Ig
// }

void TickScheduler::Start(MoveOnlyFunction<void(TickType)> fn)
{
    if(!fn)
        WFX::Utils::Logger::GetInstance().Fatal("[TickScheduler]: Callback 'fn' was null");

    callback_ = std::move(fn);

    running_ = true;
    worker_ = std::thread(&TickScheduler::Run, this);
}

void TickScheduler::Stop()
{
    running_ = false;
    if(worker_.joinable()) worker_.join();
}

constexpr TickScheduler::TickType TickScheduler::GetCurrentTick() const
{
    return tick_.load(std::memory_order_relaxed);
}

constexpr bool TickScheduler::IsExpired(TickType now, TickType then, TickType timeout) const
{
    return static_cast<TickType>(now - then) >= timeout;
}

void TickScheduler::Run()
{
    using namespace std::chrono;
    auto nextTick = steady_clock::now();

    while(running_) {
        nextTick += seconds(1);
        std::this_thread::sleep_until(nextTick);

        TickType currentTick = ++tick_; // uint16_t wraps naturally

        if(currentTick % 5 == 0)
            callback_(currentTick);
    }
}

} // namespace WFX::Http
