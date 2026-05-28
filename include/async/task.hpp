// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_CXX_ASYNC_TASK_HPP
#define WFX_INC_CXX_ASYNC_TASK_HPP

#include "promise.hpp"

namespace WFX::Async {

// Task is fire and forget. 'final_suspend' destroys the coroutine frame
template <typename T> struct [[nodiscard]] Task {
    using promise_type = Promise<T>;
    using HandleType = std::coroutine_handle<promise_type>;

    HandleType handle_;

public:
    Task(HandleType h) : handle_(h)
    {}
    Task(Task&& o) noexcept : handle_(o.handle_)
    {
        o.handle_ = nullptr;
    }
    ~Task() = default;

    // No copying allowed
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

public:
    void Resume()
    {
        if(handle_ && !handle_.done())
            handle_.resume();
    }

    void SetCompletion(AsyncCompleteFn cb, void* ud)
    {
        if(handle_) {
            handle_.promise().onDone_ = cb;
            handle_.promise().onDoneUd_ = ud;
        }
    }
};

// 'get_return_object' definitions
inline Task<void> Promise<void>::get_return_object()
{
    return Task<void>{std::coroutine_handle<Promise<void>>::from_promise(*this)};
}

inline Task<Shared::MiddlewareAction> Promise<Shared::MiddlewareAction>::get_return_object()
{
    return Task<Shared::MiddlewareAction>{
        std::coroutine_handle<Promise<Shared::MiddlewareAction>>::from_promise(*this)};
}

} // namespace WFX::Async

#endif // WFX_INC_CXX_ASYNC_TASK_HPP