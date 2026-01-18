#ifndef WFX_INC_HTTP_HELPER_HPP
#define WFX_INC_HTTP_HELPER_HPP

#include "async/task.hpp"
#include "http/common/http_route_common.hpp"

// vvv Http Stuff vvv
template<typename Lambda>
HttpCallbackType MakeHttpCallbackFromLambda(Lambda&& cb)
{
    using Request = WFX::Http::HttpRequest;

    // Async lambda, wrap automatically
    if constexpr(std::is_invocable_r_v<AsyncVoid, Lambda, Request&, Response>)
        return AsyncCallbackType{std::forward<Lambda>(cb)};

    // Sync lambda
    else if constexpr(std::is_invocable_r_v<void, Lambda, Request&, Response>)
        return SyncCallbackType{std::forward<Lambda>(cb)};

    else
        static_assert(
            std::false_type::value,
            "[UserSide:Http-Callback]: Invalid route callback. Expected one of:\n"
            "  - Sync callback:  void(Request&, Response)\n"
            "  - Async callback: AsyncVoid(Request&, Response)\n"
        );
}

// vvv Middleware Stuff vvv
template<typename Lambda>
inline HttpMiddlewareType MakeMiddlewareEntry(Lambda&& cb)
{
    using Request = WFX::Http::HttpRequest;

    // Sync middleware
    if constexpr(std::is_invocable_r_v<MiddlewareAction, Lambda, Request&, Response>)
        return SyncMiddlewareType{std::forward<Lambda>(cb)};

    // Async middleware
    else if constexpr(std::is_invocable_r_v<AsyncMiddlewareAction, Lambda, Request&, Response>)
        return AsyncMiddlewareType{std::forward<Lambda>(cb)};

    else
        // Function passed in does not match any of the signatures :(
        static_assert(
            std::false_type::value,
            "[UserSide:Http-Middleware]: Invalid middleware type. Expected either:\n"
            "  - A sync middleware: MiddlewareAction(Request&, Response)\n"
            "  - An async middleware: AsyncMiddlewareAction(Request&, Response)\n"
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