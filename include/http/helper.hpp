#ifndef WFX_INC_HTTP_HELPER_HPP
#define WFX_INC_HTTP_HELPER_HPP

/*
 * Bunch of stuff to help with other stuff
 * More to be added here, someday
 */

#include "async/runtime.hpp"
#include "http/common/http_route_common.hpp"

// vvv Http Stuff vvv
template<typename Lambda>
HttpCallbackType MakeHttpCallbackFromLambda(Lambda&& cb)
{
    using Request = WFX::Http::HttpRequest;

    // Sync lambda
    if constexpr(std::is_invocable_r_v<void, Lambda, Request&, Response&>)
        return SyncCallbackType{std::forward<Lambda>(cb)};

    // Async lambda, wrap automatically
    else if constexpr(std::is_invocable_r_v<void, Lambda, AsyncPtr, Request&, Response&>)
        return AsyncCallbackType{
            [cb = std::forward<Lambda>(cb)](Request& req, Response& res) mutable -> AsyncPtr {
                return Async::MakeAsync<void>(std::forward<Lambda>(cb), std::ref(req), res); 
            }
        };

    else
        static_assert(
            std::false_type::value,
            "[UserSide:Http-Callback]: Invalid route callback. Expected one of:\n"
            "  - Sync callback:  void(Request&, Response&)\n"
            "  - Async callback: AsyncPtr(Request&, Response&)\n"
        );
}

// vvv Middleware Stuff vvv
template<typename Lambda>
inline HttpMiddlewareType MakeMiddlewareEntry(Lambda&& cb)
{
    using Request = WFX::Http::HttpRequest;
    using RawT    = std::decay_t<Lambda>;

    // Sync middleware
    if constexpr(std::is_invocable_r_v<MiddlewareAction, RawT, Request&, Response&>)
        return SyncMiddlewareType{std::forward<Lambda>(cb)};

    // Async middleware
    else if constexpr(std::is_invocable_r_v<void, RawT, AsyncPtr, Request&, Response&>) {
        return AsyncMiddlewareType{
            [cb = std::forward<Lambda>(cb)](Request& req, Response& res) mutable -> AsyncPtr {
                return Async::MakeAsync<MiddlewareAction>(std::forward<Lambda>(cb), std::ref(req), res); 
            }
        };
    }

    else
        // Function passed in does not match any of the signatures :(
        static_assert(
            std::false_type::value,
            "[UserSide:Http-Middleware]: Invalid middleware type. Expected either:\n"
            "  - A sync middleware: MiddlewareAction(Request&, Response&)\n"
            "  - An async middleware: void(AsyncPtr, Request&, Response&)\n"
        );
}

template<typename... Lambda>
inline HttpMiddlewareStack MakeMiddlewareFromFunctions(Lambda&&... mws)
{
    HttpMiddlewareStack stack;
    stack.reserve(sizeof...(mws));

    (stack.emplace_back(
        MakeMiddlewareEntry(std::forward<Lambda>(mws))
    ), ...);

    return stack;
}

#endif // WFX_INC_HTTP_HELPER_HPP