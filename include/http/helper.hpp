#ifndef WFX_INC_HTTP_HELPER_HPP
#define WFX_INC_HTTP_HELPER_HPP

/*
 * Bunch of stuff to help with other stuff
 * More to be added here, someday
 */

#include "async/routes.hpp"
#include "http/common/http_route_common.hpp"

// vvv Helper Stuff vvv
template<class>
inline constexpr bool always_false = false;

// vvv Main Stuff vvv
template<typename Lambda>
HttpCallbackType MakeHttpCallbackFromLambda(Lambda&& cb)
{
    using RequestRef  = WFX::Http::HttpRequest&;
    using ResponseRef = Response&;

    // Sync lambda
    if constexpr(std::is_invocable_r_v<void, Lambda, RequestRef, ResponseRef>)
        return SyncCallbackType{std::forward<Lambda>(cb)};

    // Async lambda, wrap automatically
    else if constexpr(std::is_invocable_r_v<void, Lambda, AsyncPtr, RequestRef, ResponseRef>)
        return AsyncCallbackType{
            [cb = std::forward<Lambda>(cb)](RequestRef req, ResponseRef res) mutable -> AsyncPtr {
                return Async::MakeAsync(std::forward<Lambda>(cb), std::ref(req), res); 
            }
        };

    else
        static_assert(always_false<Lambda>, "Lambda must match either sync or async signature");
}

template<typename... MWs>
inline MiddlewareStack MakeMiddlewareFromFunctions(MWs&&... mws)
{
    MiddlewareStack stack;
    stack.reserve(sizeof...(mws));
    (stack.emplace_back(std::forward<MWs>(mws)), ...);
    return stack;
}

#endif // WFX_INC_HTTP_HELPER_HPP