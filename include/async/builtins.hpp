#ifndef WFX_INC_CXX_ASYNC_BUILTINS_HPP
#define WFX_INC_CXX_ASYNC_BUILTINS_HPP

#include "promise.hpp"
#include "core/core.hpp"

namespace Async {

struct SleepForAwaitable {
public: // Storage
    std::uint32_t delayMs = 0;
    Async::Status status  = Async::Status::NONE;

public: // Main setup
    // Always suspend
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        bool scheduled = __WFXApi->GetAsyncAPIV1()->RegisterAsyncTimer(
                            __WFXApi->GetHttpAPIV1()->GetGlobalPtrData(),
                            delayMs
                        );

        // On failure, resume the coroutine so user can handle the error
        if(!scheduled) {
            status = Async::Status::TIMER_FAILURE;
            h.resume();
        }
    }

    // Return status
    Async::Status await_resume() const noexcept { return status; }
};

inline SleepForAwaitable SleepFor(std::uint32_t delayMs)
{
    return SleepForAwaitable{delayMs};
}

} // namespace Async

#endif // WFX_INC_CXX_ASYNC_BUILTINS_HPP