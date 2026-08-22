// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_CXX_ASYNC_PROMISE_HPP
#define WFX_INC_CXX_ASYNC_PROMISE_HPP

#include "core/core.hpp"
#include "shared/abis/types.hpp"
#include "shared/utils/deferred_value.hpp"
#include "shared/utils/memory.hpp"
#include <coroutine>
#include <type_traits>

namespace WFX::Async {

using WFX::Shared::AsyncCompleteFn;
using WFX::Shared::AsyncResult;
using WFX::Shared::AsyncStatus;

template <typename T> struct Task;

// Shared base, allocator + initial_suspend
struct BasePromise {
    AsyncCompleteFn onDone = nullptr;
    void* onDoneUd = nullptr;

    // Only gets set when another coroutine 'co_await's this one, by Task<T>::await_suspend.
    // Stays null for the usual case of the engine driving this coroutine directly through
    // Resume()/SetCompletion(), and final_suspend below just falls through to that old path
    std::coroutine_handle<> continuation = nullptr;

public:
    void* operator new(std::size_t size)
    {
        return Shared::Alloc(size);
    }
    void operator delete(void* ptr)
    {
        Shared::Free(ptr);
    }

public:
    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    std::suspend_always initial_suspend() noexcept
    {
        return {};
    }
};

// The 'final_suspend' every Promise<T> below returns. Either hand control straight back to
// whatever 'co_await'ed us, or, when nobody did and the engine is driving this coroutine
// directly, convert into an AsyncResult and fire onDone. PromiseT supplies that conversion
// via BuildAsyncResult(), everything else here is identical for every T.
template <typename PromiseT> struct FinalSuspendAwaiter {
    PromiseT* p;

public:
    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    bool await_ready() noexcept
    {
        return false;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept
    {
        if(p->continuation)
            return p->continuation;

        const AsyncResult result = p->BuildAsyncResult();

        auto cb = p->onDone;
        auto ud = p->onDoneUd;

        h.destroy();

        if(cb)
            cb(ud, result);

        return std::noop_coroutine();
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    void await_resume() noexcept
    {}
};

// Generic Promise<T>. Covers MiddlewareAction and ConnectResult (the two types the engine ever
// drives directly besides void) as well as any T only ever 'co_await'ed by another coroutine,
// e.g. Async::Task<PgNotifyResult>. BuildAsyncResult below is the only place that needs to know
// which of those T actually is.
template <typename T> struct Promise : BasePromise {
    Shared::DeferredValue<T> value;
    AsyncStatus status = AsyncStatus::NONE;

public:
    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    void return_value(T v) noexcept
    {
        value.Emplace(std::move(v));
        status = AsyncStatus::COMPLETED;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    void unhandled_exception() noexcept
    {
        status = AsyncStatus::INTERNAL_FAILURE;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    FinalSuspendAwaiter<Promise> final_suspend() noexcept
    {
        return {this};
    }

    AsyncResult BuildAsyncResult() noexcept
    {
        if constexpr(std::is_same_v<T, Shared::MiddlewareAction>)
            return AsyncResult{.data = nullptr, .dataLen = 0, .action = value.Get(), .status = status};

        else if constexpr(std::is_same_v<T, Shared::ConnectResult>)
            return AsyncResult{.data = nullptr, .dataLen = 0, .connectResult = value.Get(), .status = status};

        // Only MiddlewareAction/ConnectResult Tasks are ever engine-driven directly (see
        // WFX::MwCoro / WFX::EpCoro), every other T is always 'co_await'ed by another
        // coroutine, so 'continuation' above is never null and this branch never runs.
        else
            return AsyncResult{.data = nullptr,
                               .dataLen = 0,
                               .action = Shared::MiddlewareAction::CONTINUE,
                               .status = AsyncStatus::INTERNAL_FAILURE};
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    Task<T> get_return_object();
};

// void can't be stored in a DeferredValue<T> or taken as a return_value(T) parameter, so it
// stays its own specialization, not by choice but because the language requires it.
template <> struct Promise<void> : BasePromise {
    AsyncStatus status = AsyncStatus::NONE;

public:
    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    void return_void() noexcept
    {
        status = AsyncStatus::COMPLETED;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    void unhandled_exception() noexcept
    {
        status = AsyncStatus::INTERNAL_FAILURE;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    FinalSuspendAwaiter<Promise> final_suspend() noexcept
    {
        return {this};
    }

    AsyncResult BuildAsyncResult() noexcept
    {
        return AsyncResult{.data = nullptr,
                           .dataLen = 0,
                           .action = Shared::MiddlewareAction::CONTINUE,
                           .status = status};
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    Task<void> get_return_object();
};

} // namespace WFX::Async

#endif // WFX_INC_CXX_ASYNC_PROMISE_HPP
