#ifndef WFX_INC_HTTP_HELPER_HPP
#define WFX_INC_HTTP_HELPER_HPP

/*
 * Bunch of stuff to help with other stuff
 * More to be added here, someday
 */

#include "async/runtime.hpp"
#include "http/common/http_route_common.hpp"

// vvv Helper Stuff vvv
template<class>
inline constexpr bool AlwaysFalse = false;

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
        static_assert(
            AlwaysFalse<Lambda>,
            "[UserSide:Http-Callback]: Lambda must match either sync [ void(Request&, Response&) ]"
            " or async [ void(AsyncPtr, Request&, Response&) ] signature"
        );
}

template<typename T>
inline MiddlewareEntry MakeMiddlewareEntry(T&& entry)
{
    using Request = WFX::Http::HttpRequest;
    using RawT    = std::decay_t<T>;

    if constexpr (std::is_invocable_r_v<MiddlewareAction, RawT, Request&, Response&>) {
        MiddlewareEntry e{};
        e.sm = std::forward<T>(entry);
        return e;
    }

    else if constexpr (std::is_same_v<RawT, MiddlewareEntry>)
        return std::forward<T>(entry);

    else {
        static_assert(
            AlwaysFalse<T>,
            "[UserSide:Http-Middleware]: Middleware must be either a function with signature "
            "'MiddlewareAction(Request&,Response&)' or a MiddlewareEntry struct."
        );
    }
}

template <typename... FunctionOrEntry>
inline MiddlewareStack MakeMiddlewareFromFunctions(FunctionOrEntry&&... mws)
{
    MiddlewareStack stack;
    stack.reserve(sizeof...(mws));

    (stack.emplace_back(
        MakeMiddlewareEntry(std::forward<FunctionOrEntry>(mws))
    ), ...);

    return stack;
}

#endif // WFX_INC_HTTP_HELPER_HPP