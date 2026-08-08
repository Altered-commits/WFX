// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_CXX_ASYNC_AWAITABLE_HPP
#define WFX_INC_CXX_ASYNC_AWAITABLE_HPP

#include "shared/abis/types.hpp"
#include <coroutine>

namespace WFX::Async {

using WFX::Shared::AsyncResult;
using WFX::Shared::AsyncStatus;

template <typename Derived> struct AwaitableBase {
    AsyncResult result = {nullptr, 0, Shared::MiddlewareAction::CONTINUE, AsyncStatus::NONE};
    std::coroutine_handle<> handle;

public: // 'AsyncAPI' Callback
    static void OnComplete(void* ud, AsyncResult result) noexcept
    {
        auto* self = static_cast<Derived*>(ud);
        self->result = result;
        self->handle.resume();
    }

    static void OnDestroy(void* ud) noexcept
    {
        auto* self = static_cast<Derived*>(ud);
        self->handle.destroy();
    }

public: // Always suspend
    // NOLINTNEXTLINE(readability-identifier-naming) - C++20 coroutine protocol name, fixed spelling
    bool await_ready() const noexcept
    {
        return false;
    }
};

} // namespace WFX::Async

#endif // WFX_INC_CXX_ASYNC_AWAITABLE_HPP