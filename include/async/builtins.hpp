#ifndef WFX_INC_CXX_ASYNC_BUILTINS_HPP
#define WFX_INC_CXX_ASYNC_BUILTINS_HPP

#include "awaitable.hpp"
#include "promise.hpp"
#include "core/core.hpp"

namespace WFX::Async {

struct SleepForAwaitable : public AwaitableBase<SleepForAwaitable> {
    std::uint32_t delayMs = 0;

public:
    explicit SleepForAwaitable(std::uint32_t ms) noexcept
        : AwaitableBase{}, delayMs(ms)
    {}

public:
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle_ = h;

        // Passed by engine, guaranteed
        void* connCtx   = Core::HttpApi()->GetGlobalPtrData();
        bool  scheduled = Core::AsyncApi()->RegisterAsyncTimer(
            connCtx, delayMs, {this, OnComplete, OnDestroy}
        );

        // On failure, resume the coroutine so user can handle the error
        if(!scheduled) {
            result_.status = WFX::Shared::AsyncStatus::TIMER_FAILURE;
            return false;
        }

        // Suspend it till engine resumes
        return true;
    }

    // Return status
    AsyncStatus await_resume() const noexcept { return result_.status; }
};

inline SleepForAwaitable SleepFor(std::uint32_t delayMs)
{
    return SleepForAwaitable{delayMs};
}

} // namespace WFX::Async

#endif // WFX_INC_CXX_ASYNC_BUILTINS_HPP