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
template<typename T>
inline MiddlewareEntry MakeMiddlewareEntry(
    T&& funcOrEntry, std::uint8_t handle = static_cast<std::uint8_t>(MiddlewareType::LINEAR)
) {
    using Request = WFX::Http::HttpRequest;
    using RawT    = std::decay_t<T>;

    // User passed a MiddlewareEntry directly, return it as is
    if constexpr(std::is_same_v<RawT, MiddlewareEntry>)
        return std::forward<T>(funcOrEntry);

    // Create the entry and return it
    MiddlewareEntry entry{};
    entry.handled = handle;

    // Sync middleware
    if constexpr(std::is_invocable_r_v<MiddlewareAction, RawT, Request&, Response&, MiddlewareMeta>) {
        entry.mw = SyncMiddlewareType{std::forward<T>(funcOrEntry)};
        return entry;
    }

    // Async middleware
    else if constexpr(std::is_invocable_r_v<void, RawT, AsyncPtr, Request&, Response&, MiddlewareMeta>) {
        entry.mw = AsyncMiddlewareType{
            [
                cb = std::forward<T>(funcOrEntry)
            ]
            (Request& req, Response& res, MiddlewareMeta meta) mutable -> AsyncPtr {
                return Async::MakeAsync<MiddlewareAction>(std::forward<T>(cb), std::ref(req), res, meta); 
            }
        };
        return entry;
    }

    else
        // Function passed in does not match any of the signatures :(
        static_assert(
            std::false_type::value,
            "[UserSide:Http-Middleware]: Invalid middleware type. Expected either:\n"
            "  - A sync middleware:  MiddlewareAction(Request&, Response&, MiddlewareMeta)\n"
            "  - An async middleware: void(AsyncPtr, Request&, Response&, MiddlewareMeta)\n"
            "  - A MiddlewareEntry struct\n"
        );
}

template<typename... FunctionOrEntry>
inline MiddlewareStack MakeMiddlewareFromFunctions(FunctionOrEntry&&... mws)
{
    MiddlewareStack stack;
    stack.reserve(sizeof...(mws));

    (stack.emplace_back(
        MakeMiddlewareEntry(std::forward<FunctionOrEntry>(mws))
    ), ...);

    return stack;
}

template<typename... Ts>
inline constexpr std::uint8_t MakeMiddlewareHandle(Ts... ts)
{
    static_assert((std::is_same_v<Ts, MiddlewareType> && ...),
                "[UserSide:Http-Middleware]: 'MakeMiddlewareHandle' only accepts MiddlewareType");

    return (static_cast<std::uint8_t>(ts) | ...);
}

#endif // WFX_INC_HTTP_HELPER_HPP