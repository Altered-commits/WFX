#ifndef WFX_INC_HTTP_HELPER_HPP
#define WFX_INC_HTTP_HELPER_HPP

#include "request.hpp"
#include "response.hpp"
#include "async/task.hpp"
#include "core/core.hpp"
#include "shared/abis/types.hpp"

namespace WFX::Http {

// Helper for static_assert
template<typename T>
struct AlwaysFalse : std::false_type {};

template<typename Lambda>
Shared::RouteCallback MakeRouteCallback(Lambda&& cb)
{
    // Async route: Lambda returns 'Async::Void'
    if constexpr(std::is_invocable_r_v<Async::Void, Lambda, Request, Response>) {
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

        Shared::RouteCallback rc;
        rc.kind  = Shared::CallbackKind::ASYNC;
        rc.async = [](Request req, Response res, Shared::AsyncCompleteFn onDone, void* onDoneUd)
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

        Shared::RouteCallback rc;
        rc.kind = Shared::CallbackKind::SYNC;
        rc.sync = [](Request req, Response res) {
            fn(req, res);
        };

        return rc;
    }

    else {
        static_assert(
            AlwaysFalse<Lambda>::value,
            "[WFX]: Invalid route callback. Expected one of:\n"
            "  - void(WFX::Http::Request, WFX::Http::Response)\n"
            "  - Async::Void(WFX::Http::Request, WFX::Http::Response)\n"
        );
    }
}

template<typename Lambda>
Shared::MwCallback MakeMwCallback(Lambda&& cb)
{
    // Async middleware: returns 'Async::MiddlewareAction'
    if constexpr(std::is_invocable_r_v<Async::MiddlewareAction, Lambda, Request, Response>) {
        static auto fn = cb;

        Shared::MwCallback mc;
        mc.kind  = Shared::CallbackKind::ASYNC;
        mc.async = [](Request req, Response res, Shared::AsyncCompleteFn onDone, void* onDoneUd)
        {
            auto task = fn(req, res);
            task.SetCompletion(onDone, onDoneUd);
            task.Resume();
        };

        return mc;
    }

    // Sync middleware: returns 'MiddlewareAction'
    else if constexpr(std::is_invocable_r_v<Shared::MiddlewareAction, Lambda, Request, Response>) {
        static auto fn = cb;

        Shared::MwCallback mc;
        mc.kind = Shared::CallbackKind::SYNC;
        mc.sync = [](Request req, Response res) -> Shared::MiddlewareAction {
            return fn(req, res);
        };

        return mc;
    }

    else {
        static_assert(
            AlwaysFalse<Lambda>::value,
            "[WFX]: Invalid middleware callback. Expected one of:\n"
            "  - Shared::MiddlewareAction(WFX::Http::Request, WFX::Http::Response)\n"
            "  - Async::MiddlewareAction(WFX::Http::Request, WFX::Http::Response)\n"
        );
    }
}

// Variadic helper to build a 'MwCallback' array for per-route middleware
template<typename... Lambdas>
struct MwCallbackArray {
    Shared::MwCallback entries[sizeof...(Lambdas)];

public:
    MwCallbackArray(Lambdas&&... mws)
        : entries{ MakeMwCallback(std::forward<Lambdas>(mws))... }
    {}

public:
    const Shared::MwCallback* Data()  const { return entries; }
    std::size_t               Count() const { return sizeof...(Lambdas); }
};

template<typename... Lambdas>
MwCallbackArray<Lambdas...> MakeMiddleware(Lambdas&&... mws) {
    return MwCallbackArray<Lambdas...>{std::forward<Lambdas>(mws)...};
}

} // namespace WFX::Http

#endif // WFX_INC_HTTP_HELPER_HPP