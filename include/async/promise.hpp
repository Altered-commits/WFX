#ifndef WFX_INC_CXX_ASYNC_PROMISE_HPP
#define WFX_INC_CXX_ASYNC_PROMISE_HPP

#include <coroutine>
#include <utility>
#include <cstdint>

namespace Async {

enum class Status : std::uint8_t {
    NONE = 0,
    COMPLETED,     // Mostly for internal use
    TIMER_FAILURE,
    IO_FAILURE,
    INTERNAL_FAILURE
};

// Base Promise (Contains shared error storage)
struct BasePromise {
    Status error_ = Status::NONE;

    // We must manually call .resume()
    std::suspend_always initial_suspend() noexcept { return {}; }

    // Keep frame alive for result check
    std::suspend_always final_suspend() noexcept { return {}; }

    // We won't directly use exceptions, just set error code
    void unhandled_exception() noexcept { error_ = Status::INTERNAL_FAILURE; }
};

// Forward declaration
template<typename T> struct Task;

// Promise specialization (T)
template<typename T>
struct Promise : BasePromise {
    T value_{};

    // Success path: co_return T;
    void return_value(T&& v) noexcept
    {
        value_ = std::move(v);
        error_ = Status::NONE;
    }
    void return_value(const T& v) noexcept
    {
        value_ = v;
        error_ = Status::NONE;
    }

    // Failure path: co_return Status::...;
    void return_value(Status e) noexcept
    {
        error_ = e;
        // value_ remains default
    }

    Task<T> get_return_object();
};

// Promise specialization (Void)
template<>
struct Promise<void> : BasePromise {
    void return_void() noexcept {}

    Task<void> get_return_object();
};

} // namespace Async

#endif // WFX_INC_CXX_ASYNC_PROMISE_HPP