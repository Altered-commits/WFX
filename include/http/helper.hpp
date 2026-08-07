// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_HTTP_HELPER_HPP
#define WFX_INC_HTTP_HELPER_HPP

#include "request.hpp"
#include "response.hpp"
#include "async/task.hpp"
#include "core/core.hpp"
#include "shared/abis/types.hpp"

namespace WFX::Http {

template <typename T> struct AlwaysFalse : std::false_type {};

template <typename Lambda> Shared::RouteCallback MakeRouteCallback(Lambda&& cb)
{
    // Async route: Lambda returns WFX::Coro (Task<void>)
    if constexpr(std::is_invocable_r_v<Async::Task<void>, Lambda, Request, Response>) {
        static auto GlobalFn = cb;

        Shared::RouteCallback rc;
        rc.kind = Shared::CallbackKind::ASYNC;
        rc.async = [](Request req, Response res, Shared::AsyncCompleteFn onDone, void* onDoneUd) {
            auto task = GlobalFn(req, res);
            task.SetCompletion(onDone, onDoneUd);
            task.Resume();
            // Task will go out of scope, and that is fine
            // If coroutine suspended: engine callback chain resumes it later:
            //   'final_suspend' destroys frame + fires 'onDone'
            // If coroutine finished synchronously:
            //   'final_suspend' already destroyed frame + fired 'onDone'
        };

        return rc;
    }

    // Sync route: Lambda returns void
    else if constexpr(std::is_invocable_r_v<void, Lambda, Request, Response>) {
        static auto GlobalFn = cb;

        Shared::RouteCallback rc;
        rc.kind = Shared::CallbackKind::SYNC;
        rc.sync = [](Request req, Response res) { GlobalFn(req, res); };

        return rc;
    }

    else {
        static_assert(AlwaysFalse<Lambda>::value, "[WFX]: Invalid route callback. Expected one of:\n"
                                                  "  - void(WFX::Request, WFX::Response)\n"
                                                  "  - WFX::Coro(WFX::Request, WFX::Response)\n");
    }
}

template <typename Lambda> Shared::MwCallback MakeMwCallback(Lambda&& cb)
{
    // Async middleware: returns WFX::MwCoro (Task<MiddlewareAction>)
    if constexpr(std::is_invocable_r_v<Async::Task<Shared::MiddlewareAction>, Lambda, Request, Response>) {
        static auto GlobalFn = cb;

        Shared::MwCallback mc;
        mc.kind = Shared::CallbackKind::ASYNC;
        mc.async = [](Request req, Response res, Shared::AsyncCompleteFn onDone, void* onDoneUd) {
            auto task = GlobalFn(req, res);
            task.SetCompletion(onDone, onDoneUd);
            task.Resume();
        };

        return mc;
    }

    // Sync middleware: returns WFX::MiddlewareAction
    else if constexpr(std::is_invocable_r_v<Shared::MiddlewareAction, Lambda, Request, Response>) {
        static auto GlobalFn = cb;

        Shared::MwCallback mc;
        mc.kind = Shared::CallbackKind::SYNC;
        mc.sync = [](Request req, Response res) -> Shared::MiddlewareAction { return GlobalFn(req, res); };

        return mc;
    }

    else {
        static_assert(AlwaysFalse<Lambda>::value, "[WFX]: Invalid middleware callback. Expected one of:\n"
                                                  "  - WFX::MiddlewareAction(WFX::Request, WFX::Response)\n"
                                                  "  - WFX::MwCoro(WFX::Request, WFX::Response)\n");
    }
}

// Variadic helper to build a 'MwCallback' array for per-route middleware
template <typename... Lambdas> struct MwCallbackArray {
    Shared::MwCallback entries[sizeof...(Lambdas)];

public:
    MwCallbackArray(Lambdas&&... mws) : entries{MakeMwCallback(std::forward<Lambdas>(mws))...}
    {}

public:
    const Shared::MwCallback* Data() const
    {
        return entries;
    }
    std::size_t Count() const
    {
        return sizeof...(Lambdas);
    }
};

template <typename... Lambdas> MwCallbackArray<Lambdas...> MakeMiddleware(Lambdas&&... mws)
{
    return MwCallbackArray<Lambdas...>{std::forward<Lambdas>(mws)...};
}

} // namespace WFX::Http

#endif // WFX_INC_HTTP_HELPER_HPP