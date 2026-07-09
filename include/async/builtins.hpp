// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_CXX_ASYNC_BUILTINS_HPP
#define WFX_INC_CXX_ASYNC_BUILTINS_HPP

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
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle_ = h;

        // Passed by engine, guaranteed
        void* connCtx = Core::HttpApiExt1()->GetGlobalPtrData();
        bool scheduled = Core::AsyncApiExt1()->RegisterAsyncTimer(connCtx, delayMs, {this, OnComplete, OnDestroy});

        // On failure, resume the coroutine so user can handle the error
        if(!scheduled) {
            result_.status = WFX::Shared::AsyncStatus::TIMER_FAILURE;
            return false;
        }

        // Suspend it till engine resumes
        return true;
    }

    // Return status
    AsyncStatus await_resume() const noexcept
    {
        return result_.status;
    }
};

inline SleepForAwaitable SleepFor(std::uint32_t delayMs)
{
    return SleepForAwaitable{delayMs};
}

// co_await handle.Send(...) inside onConnect coroutines
// Suspends the coroutine, calls SlotSend via the endpoint API, resumes on flush
struct SlotSendAwaitable : public AwaitableBase<SlotSendAwaitable> {
    void* slotInternal;
    const void* data;
    std::uint32_t size;

public:
    SlotSendAwaitable(void* impl, const void* d, std::uint32_t s) noexcept
        : AwaitableBase{}, slotInternal(impl), data(d), size(s)
    {}

    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle_ = h;
        Core::EndpointApiExt1()->SlotSend(slotInternal, data, size, {this, OnComplete, OnDestroy});
        return true;
    }

    Shared::SlotSendResult await_resume() const noexcept
    {
        if(result_.status != AsyncStatus::COMPLETED)
            return Shared::SlotSendResult::ERROR;

        return Shared::SlotSendResult::OK;
    }
};

// co_await handle.Receive() inside onConnect coroutines
// Suspends the coroutine, arms the slot for the next read, resumes when data arrives
// Buffer pointer is only valid until the next SlotReceive call
struct SlotReceiveResult {
    Shared::SlotReceiveStatus status;
    const char* buf;
    std::uint32_t len;
};

struct SlotReceiveAwaitable : public AwaitableBase<SlotReceiveAwaitable> {
    void* slotInternal;

public:
    explicit SlotReceiveAwaitable(void* impl) noexcept : AwaitableBase{}, slotInternal(impl)
    {}

    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle_ = h;
        Core::EndpointApiExt1()->SlotReceive(slotInternal, {this, OnComplete, OnDestroy});
        return true;
    }

    SlotReceiveResult await_resume() const noexcept
    {
        if(result_.status != AsyncStatus::COMPLETED)
            return {Shared::SlotReceiveStatus::ERROR, nullptr, 0};

        return {Shared::SlotReceiveStatus::OK, static_cast<const char*>(result_.data), result_.dataLen};
    }
};

} // namespace WFX::Async

#endif // WFX_INC_CXX_ASYNC_BUILTINS_HPP