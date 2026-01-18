#ifndef WFX_INC_CXX_ASYNC_TASK_HPP
#define WFX_INC_CXX_ASYNC_TASK_HPP

#include "promise.hpp"

namespace Async {

// vvv Generic Task (Can hold any type of coroutine) vvv
struct GenericTask {
    std::coroutine_handle<> handle_ = nullptr;

public: // Constructor
    GenericTask() = default;

public: // Main functions
    void Reset()
    {
        if(handle_) { handle_.destroy(); handle_ = nullptr; }
    }

    // Common Interface
    void     Resume()           { if(handle_ && !handle_.done()) handle_.resume(); }
    bool     IsFinished() const { return !handle_ || handle_.done(); }
    operator bool()       const { return handle_ != nullptr; }

    // Helper to get typed promise back (unsafe cast)
    template<typename PromiseType>
    PromiseType& GetPromise() const
    {
        return std::coroutine_handle<PromiseType>::from_address(handle_.address()).promise();
    }
};

// vvv Main shit vvv
template<typename T = void>
struct [[nodiscard]] Task {
    using promise_type = Promise<T>;
    using HandleType   = std::coroutine_handle<promise_type>;

    HandleType handle_;

public: // Constructors / Destructor
    explicit Task(HandleType h) : handle_(h) {}

    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept
        : handle_(other.handle_)
    {
        other.handle_ = nullptr;
    }

    Task& operator=(Task&& other) noexcept
    {
        if(this != &other) {
            if(handle_) handle_.destroy();

            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Task()
    {
        if(handle_) handle_.destroy();
    }

public: // Main functions
    void     Resume()            { if(handle_ && !handle_.done()) handle_.resume(); }
    bool     IsFinished()  const { return !handle_ || handle_.done(); }
    operator GenericTask() &&    { GenericTask g; g.handle_ = handle_; handle_ = nullptr; return g; }
    operator bool()        const { return handle_ != nullptr; }

    // For T: returns { value, status }
    std::pair<T, Status> GetResult() const
    {
        // If handle is dead/null, we can't do much, just return internal error
        if(!handle_ || !handle_.done())
            return { T{}, Status::INTERNAL_FAILURE };

        auto& p = handle_.promise();
        return { p.value_, p.error_ };
    }
};

// Void specialization (Return only Status)
template<>
struct [[nodiscard]] Task<void> {
    using promise_type = Promise<void>;
    using HandleType   = std::coroutine_handle<promise_type>;

    HandleType handle_;

public: // Constructors and Destructor
    explicit Task(HandleType h)
        : handle_(h)
    {}

    Task(Task&& other) noexcept
        : handle_(other.handle_)
    {
        other.handle_ = nullptr;
    }

    ~Task()
    {
        if(handle_) handle_.destroy();
    }

public: // Main functions
    void     Resume()            { if(handle_ && !handle_.done()) handle_.resume(); }
    bool     IsFinished()  const { return !handle_ || handle_.done(); }
    operator GenericTask() &&    { GenericTask g; g.handle_ = handle_; handle_ = nullptr; return g; }
    operator bool()        const { return handle_ != nullptr; }

    // For Void: returns Status only
    Status GetResult() const
    {
        if(!handle_ || !handle_.done())
            return Status::INTERNAL_FAILURE;

        return handle_.promise().error_;
    }
};

template<typename T>
inline Task<T> Promise<T>::get_return_object()
{
    return Task<T>{ std::coroutine_handle<Promise<T>>::from_promise(*this) };
}

inline Task<void> Promise<void>::get_return_object()
{
    return Task<void>{ std::coroutine_handle<Promise<void>>::from_promise(*this) };
}

} // namespace Async

#endif // WFX_INC_CXX_ASYNC_TASK_HPP