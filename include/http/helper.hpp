// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Altered-commits

#ifndef WFX_INC_HTTP_HELPER_HPP
#define WFX_INC_HTTP_HELPER_HPP

#include "request.hpp"
#include "response.hpp"
#include "async/task.hpp"
#include "core/core.hpp"
#include "shared/abis/types.hpp"
#include <array>
#include <tuple>
#include <utility>

namespace WFX::Http {

template <typename T> struct AlwaysFalse : std::false_type {};

// The callable lives in a static of each instantiation, so Tag must be unique to the call site (the
// registering macro passes its own struct). Keyed on the callable's type alone, plain functions of
// one signature, or closures from one factory, would all run the first one registered
template <typename Tag, typename Lambda> Shared::RouteCallback MakeRouteCallback(Lambda&& cb)
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

// Same Tag rule as MakeRouteCallback
template <typename Tag, typename Lambda> Shared::MwCallback MakeMwCallback(Lambda&& cb)
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

// A per-route middleware's Tag: its route's Tag plus its position in the list
template <typename RouteTag, std::size_t Index> struct MwListTag {};

// Per-route middleware, held as given until the route macro builds them under its own Tag
template <typename... Lambdas> struct MwCallbackArray {
    std::tuple<std::decay_t<Lambdas>...> fns;

public:
    MwCallbackArray(Lambdas&&... mws) : fns{std::forward<Lambdas>(mws)...}
    {}

public:
    template <typename RouteTag> std::array<Shared::MwCallback, sizeof...(Lambdas)> Build()
    {
        return BuildImpl<RouteTag>(std::index_sequence_for<Lambdas...>{});
    }

private:
    template <typename RouteTag, std::size_t... Index>
    std::array<Shared::MwCallback, sizeof...(Lambdas)> BuildImpl(std::index_sequence<Index...>)
    {
        return {MakeMwCallback<MwListTag<RouteTag, Index>>(std::get<Index>(fns))...};
    }
};

template <typename... Lambdas> MwCallbackArray<Lambdas...> MakeMiddleware(Lambdas&&... mws)
{
    return MwCallbackArray<Lambdas...>{std::forward<Lambdas>(mws)...};
}

} // namespace WFX::Http

#endif // WFX_INC_HTTP_HELPER_HPP