// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_CXX_ASYNC_PROMISE_HPP
#define WFX_INC_CXX_ASYNC_PROMISE_HPP

#include "core/core.hpp"
#include "shared/abis/types.hpp"
#include "shared/utils/memory.hpp"
#include <coroutine>

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

// Exists so 'Task<T>' can name 'Promise<T>' as 'promise_type'
// Left incomplete for specializations to work properly
template <typename T> struct Promise;

// Promise<void> specialization (Async Routes)
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
    auto final_suspend() noexcept
    {
        struct Completion {
            Promise* p;

            // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
            bool await_ready() noexcept
            {
                return false;
            }

            // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept
            {
                // Somebody 'co_await'ed us instead of the engine driving us directly. Jump
                // straight back into them and leave 'h' alone: Task<void>::await_resume is the
                // one that destroys it, once they've actually resumed and read whatever they need
                if(p->continuation)
                    return p->continuation;

                const AsyncResult result{nullptr, 0, Shared::MiddlewareAction::CONTINUE, p->status};

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

        return Completion{this};
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    Task<void> get_return_object();
};

// Promise<MiddlewareAction> specialization (Async Middleware)
template <> struct Promise<Shared::MiddlewareAction> : BasePromise {
    AsyncStatus status = AsyncStatus::NONE;
    Shared::MiddlewareAction value = Shared::MiddlewareAction::CONTINUE;

public:
    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    void return_value(Shared::MiddlewareAction v) noexcept
    {
        value = v;
        status = AsyncStatus::COMPLETED;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    void unhandled_exception() noexcept
    {
        status = AsyncStatus::INTERNAL_FAILURE;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    auto final_suspend() noexcept
    {
        struct Completion {
            Promise* p;

            // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
            bool await_ready() noexcept
            {
                return false;
            }

            // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept
            {
                // See Promise<void>::final_suspend's Completion::await_suspend for what this
                // branch is for
                if(p->continuation)
                    return p->continuation;

                const AsyncResult result{nullptr, 0, p->value, p->status};

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

        return Completion{this};
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    Task<Shared::MiddlewareAction> get_return_object();
};

// Promise<ConnectResult> specialization (onConnect coroutines)
template <> struct Promise<Shared::ConnectResult> : BasePromise {
    AsyncStatus status = AsyncStatus::NONE;
    Shared::ConnectResult value = Shared::ConnectResult::FATAL;

public:
    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    void return_value(Shared::ConnectResult v) noexcept
    {
        value = v;
        status = AsyncStatus::COMPLETED;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    void unhandled_exception() noexcept
    {
        status = AsyncStatus::INTERNAL_FAILURE;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    auto final_suspend() noexcept
    {
        struct Completion {
            Promise* p;

            // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
            bool await_ready() noexcept
            {
                return false;
            }

            // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept
            {
                // See Promise<void>::final_suspend's Completion::await_suspend for what this
                // branch is for
                if(p->continuation)
                    return p->continuation;

                AsyncResult result{nullptr, 0, {}, p->status};
                result.connectResult = p->value;

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

        return Completion{this};
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    Task<Shared::ConnectResult> get_return_object();
};

} // namespace WFX::Async

#endif // WFX_INC_CXX_ASYNC_PROMISE_HPP
