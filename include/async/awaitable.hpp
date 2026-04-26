#ifndef WFX_INC_CXX_ASYNC_AWAITABLE_HPP
#define WFX_INC_CXX_ASYNC_AWAITABLE_HPP

#include "shared/abis/types.hpp"
#include <coroutine>

namespace WFX::Async {

using WFX::Shared::AsyncResult;
using WFX::Shared::AsyncStatus;

template<typename Derived>
struct AwaitableBase {
    AsyncResult result_ = { nullptr, 0, Shared::MiddlewareAction::CONTINUE, AsyncStatus::NONE };
    std::coroutine_handle<> handle_;

public: // 'AsyncAPI' Callback
    static void OnComplete(void* ud, AsyncResult result) noexcept
    {
        auto* self    = static_cast<Derived*>(ud);
        self->result_ = result;
        self->handle_.resume();
    }

    static void OnDestroy(void* ud) noexcept
    {
        auto* self = static_cast<Derived*>(ud);
        self->handle_.destroy();
    }

public: // Always suspend
    bool await_ready() const noexcept { return false; }
};

} // namespace WFX::Async

#endif // WFX_INC_CXX_ASYNC_AWAITABLE_HPP