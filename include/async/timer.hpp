// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_CXX_ASYNC_TIMER_HPP
#define WFX_INC_CXX_ASYNC_TIMER_HPP

#include "awaitable.hpp"
#include "promise.hpp"
#include "core/core.hpp"

namespace WFX::Async {

struct SleepForAwaitable : public AwaitableBase<SleepForAwaitable> {
    std::uint32_t delayMs = 0;

public:
    explicit SleepForAwaitable(std::uint32_t ms) noexcept : AwaitableBase{}, delayMs(ms)
    {}

public:
    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle = h;

        // Passed by engine, guaranteed
        void* connCtx = Core::HttpApiExt1()->getGlobalPtrData();
        const bool scheduled =
            Core::AsyncApiExt1()->registerAsyncTimer(connCtx, delayMs, {this, OnComplete, OnDestroy});

        // On failure, resume the coroutine so user can handle the error
        if(!scheduled) {
            result.status = WFX::Shared::AsyncStatus::TIMER_FAILURE;
            return false;
        }

        // Suspend it till engine resumes
        return true;
    }

    // Return status
    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    AsyncStatus await_resume() const noexcept
    {
        return result.status;
    }
};

inline SleepForAwaitable SleepFor(std::uint32_t delayMs)
{
    return SleepForAwaitable{delayMs};
}

} // namespace WFX::Async

#endif // WFX_INC_CXX_ASYNC_TIMER_HPP
