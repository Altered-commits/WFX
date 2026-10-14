// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_CXX_ASYNC_TASK_HPP
#define WFX_INC_CXX_ASYNC_TASK_HPP

#include "promise.hpp"
#include <exception>
#include <type_traits>

namespace WFX::Async {

// Task is fire and forget when the engine drives it directly ('final_suspend' destroys the
// coroutine frame and fires 'onDone'). It can also be 'co_await'ed from another coroutine, see
// await_ready/await_suspend/await_resume below. When that happens 'onDone' never gets touched,
// the frame instead gets destroyed by await_resume once the awaiting coroutine reads its result
template <typename T> struct [[nodiscard]] Task {
    using promise_type = Promise<T>;
    using HandleType = std::coroutine_handle<promise_type>;

    HandleType handle;

public:
    Task(HandleType h) : handle(h)
    {}
    Task(Task&& o) noexcept : handle(o.handle)
    {
        o.handle = nullptr;
    }
    ~Task() = default;

    // No copying allowed
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

public:
    void Resume()
    {
        if(handle && !handle.done())
            handle.resume();
    }

    void SetCompletion(AsyncCompleteFn cb, void* ud)
    {
        if(handle) {
            handle.promise().onDone = cb;
            handle.promise().onDoneUd = ud;
        }
    }

public:
    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    bool await_ready() const noexcept
    {
        return false;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept
    {
        // 'handle' was created suspended (initial_suspend = suspend_always), so this is what
        // actually starts it running, via symmetric transfer instead of a manual Resume() call
        handle.promise().continuation = awaiter;
        return handle;
    }

    // Not noexcept: a child that threw rethrows here, into the awaiting coroutine
    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    T await_resume()
    {
        // The child finished, so the awaiting coroutine is the one running this: the child must not
        // take it down as its frame goes (see ~BasePromise)
        auto& promise = handle.promise();
        promise.continuation = nullptr;

        // A child that threw never stored a value. Its exception carries on in the parent, exactly
        // where a normal call's would, and fails the parent only if nothing there catches it
        if(promise.exception) {
            const std::exception_ptr exception = promise.exception;
            handle.destroy();
            std::rethrow_exception(exception);
        }

        if constexpr(std::is_void_v<T>)
            handle.destroy();
        else {
            T value = std::move(promise.value.Get());
            handle.destroy();
            return value;
        }
    }
};

// 'get_return_object' definitions
// NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
inline Task<void> Promise<void>::get_return_object()
{
    return Task<void>{std::coroutine_handle<Promise<void>>::from_promise(*this)};
}

// NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
template <typename T> inline Task<T> Promise<T>::get_return_object()
{
    return Task<T>{std::coroutine_handle<Promise<T>>::from_promise(*this)};
}

} // namespace WFX::Async

#endif // WFX_INC_CXX_ASYNC_TASK_HPP