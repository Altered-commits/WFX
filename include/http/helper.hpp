#ifndef WFX_INC_HTTP_HELPER_HPP
#define WFX_INC_HTTP_HELPER_HPP

#include "request.hpp"
#include "response.hpp"
#include "async/task.hpp"
#include "core/core.hpp"
#include "shared/abis/types.hpp"

namespace WFX::Http {

using WFX::Shared::RouteCallback;
using WFX::Shared::MwCallback;
using WFX::Shared::CallbackKind;
using WFX::Shared::SyncRouteFn;
using WFX::Shared::AsyncRouteFn;
using WFX::Shared::SyncMwFn;
using WFX::Shared::AsyncMwFn;
using WFX::Shared::AsyncCompleteFn;
using WFX::Shared::AsyncResult;
using WFX::Shared::MiddlewareAction;

using AsyncVoid             = Async::Task<void>;
using AsyncMiddlewareAction = Async::Task<MiddlewareAction>;

template<typename Lambda>
RouteCallback MakeRouteCallback(Lambda&& cb)
{
    // Async route: Lambda returns 'AsyncVoid'
    if constexpr(std::is_invocable_r_v<AsyncVoid, Lambda, Request, Response>) {
        // The lambda is non-capturing, so we can store it as a constexpr/static
        // We generate a thunk that bridges the ABI signature to the user signature
        //
        // The thunk:
        //  - Wraps raw handles into Request/Response
        //  - Creates the coroutine (Task<void>)
        //  - Sets the engine's completion callback on the promise
        //  - Resumes the coroutine
        //
        // Since Lambda is non-capturing, we can call it inside the thunk directly
        // We use a static local to hold the lambda (zero-size for non-capturing)
        static auto fn = cb;

        RouteCallback rc;
        rc.kind  = CallbackKind::ASYNC;
        rc.async = [](Request req, Response res, AsyncCompleteFn onDone, void* onDoneUd)
        {
            auto task = fn(req, res);
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

    // Sync route: Lambda returns 'void'
    else if constexpr(std::is_invocable_r_v<void, Lambda, Request, Response>) {
        static auto fn = cb;

        RouteCallback rc;
        rc.kind = CallbackKind::SYNC;
        rc.sync = [](Request req, Response res) {
            fn(req, res);
        };

        return rc;
    }

    else {
        static_assert(
            std::is_invocable_r_v<void, Lambda, Request, Response>,
            "[WFX]: Invalid route callback. Expected one of:\n"
            "  - Sync:  void(WFX::Http::Request, WFX::Http::Response)\n"
            "  - Async: AsyncVoid(WFX::Http::Request, WFX::Http::Response)\n"
        );
    }
}

template<typename Lambda>
MwCallback MakeMwCallback(Lambda&& cb)
{
    // Async middleware: returns 'AsyncMiddlewareAction'
    if constexpr(std::is_invocable_r_v<AsyncMiddlewareAction, Lambda, Request, Response>) {
        static auto fn = cb;

        MwCallback mc;
        mc.kind  = CallbackKind::ASYNC;
        mc.async = [](Request req, Response res, AsyncCompleteFn onDone, void* onDoneUd)
        {
            auto task = fn(req, res);
            task.SetCompletion(onDone, onDoneUd);
            task.Resume();
        };

        return mc;
    }

    // Sync middleware: returns 'MiddlewareAction'
    else if constexpr(std::is_invocable_r_v<MiddlewareAction, Lambda, Request, Response>) {
        static auto fn = cb;

        MwCallback mc;
        mc.kind = CallbackKind::SYNC;
        mc.sync = [](Request req, Response res) -> MiddlewareAction {
            return fn(req, res);
        };

        return mc;
    }

    else {
        static_assert(
            std::is_invocable_r_v<MiddlewareAction, Lambda, Request, Response>,
            "[WFX]: Invalid middleware callback. Expected one of:\n"
            "  - Sync:  MiddlewareAction(WFX::Http::Request, WFX::Http::Response)\n"
            "  - Async: AsyncMiddlewareAction(WFX::Http::Request, WFX::Http::Response)\n"
        );
    }
}

// Variadic helper to build a 'MwCallback' array for per-route middleware
template<typename... Lambdas>
struct MwCallbackArray {
    MwCallback entries[sizeof...(Lambdas)];

public:
    MwCallbackArray(Lambdas&&... mws)
        : entries{ MakeMwCallback(std::forward<Lambdas>(mws))... }
    {}

public:
    const MwCallback* data()  const { return entries; }
    std::size_t       count() const { return sizeof...(Lambdas); }
};

template<typename... Lambdas>
MwCallbackArray<Lambdas...> MakeMiddleware(Lambdas&&... mws) {
    return MwCallbackArray<Lambdas...>{std::forward<Lambdas>(mws)...};
}

} // namespace WFX::Http

#endif // WFX_INC_HTTP_HELPER_HPP