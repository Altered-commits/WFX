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

// Every slot operation reports through AsyncResult::slotStatus. A failure that never set one
// (a generic engine-side I/O failure) still has to read as a failure, not as OK.
inline Shared::SlotStatus ResolveSlotStatus(const AsyncResult& r) noexcept
{
    if(r.status == AsyncStatus::COMPLETED)
        return Shared::SlotStatus::OK;

    return r.slotStatus == Shared::SlotStatus::OK ? Shared::SlotStatus::IO_ERROR : r.slotStatus;
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

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle = h;
        Core::EndpointApiExt1()->slotSend(slotInternal, data, size, {this, OnComplete, OnDestroy});
        return true;
    }

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    Shared::SlotStatus await_resume() const noexcept
    {
        return ResolveSlotStatus(result);
    }
};

// co_await handle.Receive(consumed) inside onConnect coroutines
// Suspends the coroutine, arms the slot for the next read, resumes when data arrives
// Buffer pointer is only valid until the next SlotReceive call
//
// 'consumed' trims that many bytes off the front of the read buffer first: how much of the
// PREVIOUS result was already used. Default 0. A handshake reading more than once (STARTTLS
// EHLO/EHLO/AUTH, ...) must pass it, or the next call redelivers the same bytes.
struct SlotReceiveResult {
    Shared::SlotStatus status;
    const char* buf;
    std::uint32_t len;
};

struct SlotReceiveAwaitable : public AwaitableBase<SlotReceiveAwaitable> {
    void* slotInternal;
    std::uint32_t consumed;

public:
    explicit SlotReceiveAwaitable(void* impl, std::uint32_t consumedBytes = 0) noexcept
        : AwaitableBase{}, slotInternal(impl), consumed(consumedBytes)
    {}

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle = h;
        Core::EndpointApiExt1()->slotReceive(slotInternal, consumed, {this, OnComplete, OnDestroy});
        return true;
    }

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    SlotReceiveResult await_resume() const noexcept
    {
        const Shared::SlotStatus status = ResolveSlotStatus(result);
        if(status != Shared::SlotStatus::OK)
            return {status, nullptr, 0};

        return {status, static_cast<const char*>(result.data), result.dataLen};
    }
};

// co_await handle.UpgradeToTLS() inside onConnect coroutines
// For protocols that negotiate encryption in-band: the probe runs on the raw socket via
// Send/Receive, then this wraps the same connection in TLS before the rest of the handshake.
struct SlotUpgradeTlsAwaitable : public AwaitableBase<SlotUpgradeTlsAwaitable> {
    void* slotInternal;

public:
    explicit SlotUpgradeTlsAwaitable(void* impl) noexcept : AwaitableBase{}, slotInternal(impl)
    {}

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    bool await_suspend(std::coroutine_handle<> h) noexcept
    {
        handle = h;
        Core::EndpointApiExt1()->slotUpgradeTls(slotInternal, {this, OnComplete, OnDestroy});
        return true;
    }

    // NOLINTNEXTLINE(readability-identifier-naming): C++20 coroutine protocol name, fixed spelling.
    Shared::SlotStatus await_resume() const noexcept
    {
        return ResolveSlotStatus(result);
    }
};

} // namespace WFX::Async

#endif // WFX_INC_CXX_ASYNC_BUILTINS_HPP