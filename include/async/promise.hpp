#ifndef WFX_INC_CXX_ASYNC_PROMISE_HPP
#define WFX_INC_CXX_ASYNC_PROMISE_HPP

#include "core/core.hpp"
#include "shared/abis/types.hpp"
#include <coroutine>

namespace WFX::Async {

using WFX::Shared::AsyncResult;
using WFX::Shared::AsyncStatus;
using WFX::Shared::AsyncCompleteFn;

template<typename T> struct Task;

// Shared base, allocator + initial_suspend
struct BasePromise {
    AsyncCompleteFn onDone_   = nullptr;
    void*           onDoneUd_ = nullptr;

public:
    void* operator new(std::size_t size) { return Core::MemoryApi()->Alloc(size); }
    void  operator delete(void* ptr)     { Core::MemoryApi()->Free(ptr); }

public:
    std::suspend_always initial_suspend() noexcept { return {}; }
};

// Exists so 'Task<T>' can name 'Promise<T>' as 'promise_type'
// Left incomplete for specializations to work properly
template<typename T>
struct Promise;

// Promise<void> specialization (Async Routes)
template<>
struct Promise<void> : BasePromise {
    AsyncStatus status_ = AsyncStatus::NONE;

public:
    void return_void() noexcept
    {
        status_ = AsyncStatus::COMPLETED;
    }

    void unhandled_exception() noexcept
    {
        status_ = AsyncStatus::INTERNAL_FAILURE;
    }

    auto final_suspend() noexcept
    {
        struct Completion {
            Promise* p;

            bool await_ready() noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) noexcept
            {
                AsyncResult result {
                    nullptr, 0,
                    Shared::MiddlewareAction::CONTINUE,
                    p->status_
                };

                auto cb = p->onDone_;
                auto ud = p->onDoneUd_;

                h.destroy();

                if(cb) cb(ud, result);
            }

            void await_resume() noexcept {}
        };

        return Completion{this};
    }

    Task<void> get_return_object();
};

// Promise<MiddlewareAction> specialization (Async Middleware)
template<>
struct Promise<Shared::MiddlewareAction> : BasePromise {
    AsyncStatus              status_ = AsyncStatus::NONE;
    Shared::MiddlewareAction value_  = Shared::MiddlewareAction::CONTINUE;

public:
    void return_value(Shared::MiddlewareAction v) noexcept
    {
        value_  = v;
        status_ = AsyncStatus::COMPLETED;
    }

    void unhandled_exception() noexcept
    {
        status_ = AsyncStatus::INTERNAL_FAILURE;
    }

    auto final_suspend() noexcept
    {
        struct Completion {
            Promise* p;

            bool await_ready() noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) noexcept
            {
                AsyncResult result {
                    nullptr, 0,
                    p->value_,
                    p->status_
                };

                auto cb = p->onDone_;
                auto ud = p->onDoneUd_;

                h.destroy();

                if(cb) cb(ud, result);
            }

            void await_resume() noexcept {}
        };

        return Completion{this};
    }

    Task<Shared::MiddlewareAction> get_return_object();
};

// Useful aliases
using Void             = Async::Task<void>;
using MiddlewareAction = Async::Task<Shared::MiddlewareAction>;

} // namespace WFX::Async

#endif // WFX_INC_CXX_ASYNC_PROMISE_HPP