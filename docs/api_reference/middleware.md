# Middleware

Middleware in WFX provides a mechanism to intercept and control request processing **before a route handler may be invoked**, including the ability to short-circuit execution entirely.  
Typical use cases include authentication, authorization, logging, request preprocessing, and early rejection of requests.

Middleware can be registered globally or per-route. This page documents both **sync and async middleware**.

!!! important
    Middleware requires the user to always include the following header at the top of the file:
    ```cpp
    #include <wfx/http.hpp>
    ```

---

## Middleware Return Value

Every middleware in WFX must return a value that determines how request processing continues.  
This return value is represented by the `MiddlewareAction` enum and is common to all middleware, regardless of where it is used.

```cpp
enum class MiddlewareAction : std::uint8_t {
    CONTINUE,
    BREAK,
    SKIP_NEXT
};
```

**Action Semantics**:

- **`CONTINUE`**  
    Proceeds to the next middleware in the chain. If no middleware remains, request handling continues to the user route handler.

- **`BREAK`**  
    Terminates middleware execution immediately. No further middleware or route handler will be executed.

- **`SKIP_NEXT`**  
    Skips the *immediately following* middleware in the chain, if one exists.  
    If the current middleware is `A`, the next middleware `B` is skipped and execution continues with `C` (if present).  
    If there is no next middleware to skip, execution continues normally.

!!! important
    Although middleware internally operates on the `MiddlewareAction` enum, user code will typically use the exported `WFX` constexpr aliases instead.

    Preferred usage:

    ```cpp
    return WFX::MwContinue;
    return WFX::MwSkipNext;
    return WFX::MwBreak;
    ```

    Instead of:

    ```cpp
    return MiddlewareAction::CONTINUE;
    return MiddlewareAction::SKIP_NEXT;
    return MiddlewareAction::BREAK;
    ```

    The aliases are provided for cleaner syntax consistency across the framework and are the recommended public-facing API surface.

## Basic Middleware

Middleware must be registered before it can be used by any route.  
Registration is done using macros and follows the same deferred initialization model as routes.

**Example**:

```cpp
// Using a lambda
WFX_MIDDLEWARE("auth", [](WFX::Request req, WFX::Response res) {
    /* ... */
    return WFX::MwContinue; // mandatory
});

// Using a function
WFX::MiddlewareAction AuthMiddleware(WFX::Request req, WFX::Response res)
{
    /* ... */
    return WFX::MwContinue; // mandatory
}

WFX_MIDDLEWARE("auth", AuthMiddleware);
```

The above code:

- Registers a middleware under a string identifier.
- Registration occurs during static initialization and is finalized at engine startup.
- The name is used to define the execution order of middleware via the `[Project] middleware_list` section in `wfx.toml`.

!!! note
    Middleware registration and configuration follow these rules:

    1. If a middleware is defined in user code but **not listed** in `[Project] middleware_list`, the server fails to start with a fatal error.

    2. If a middleware name is listed in `[Project] middleware_list` but **no corresponding middleware is registered in user code**, the server also fails to start with a fatal error.

    3. Middleware **names must be unique**:
        
        - Duplicate names in `[Project] middleware_list` result in a fatal error.
        - Duplicate middleware registrations in user code also result in a fatal error.

    4. Middleware **executes strictly in the order specified** in `[Project] middleware_list`.

## Async Middleware

Async middleware allows middleware logic to suspend execution without blocking the event loop.  
This is intended for operations that may **suspend execution**, such as time-based delays, I/O-bound work (database queries, external API calls), rate limiting, or deferred validation logic.  
Async middleware follows the **same registration, ordering, and return semantics** as synchronous middleware.  
The only difference is that execution occurs inside a coroutine.

**Example**:
```cpp
/*
 * NOTE: This header is mandatory when using any builtin async utilities-
 *       -such as functions like 'SleepFor'. It also brings in the core-
 *       -async machinery, including 'WFX::MwCoro' and related types
 */
#include <wfx/http.hpp>
#include <wfx/async.hpp>

WFX_MIDDLEWARE("RequestCooldown", [](WFX::Request _, WFX::Response res) -> WFX::MwCoro {
    auto err = co_await WFX::SleepFor(2000);

    if(err != WFX::AsyncOk) {
        res.Status(WFX::HttpStatus::INTERNAL_SERVER_ERROR)
            .SendText("Middleware Failed to sleep for 2 seconds :(");

        co_return WFX::MwBreak;
    }

    co_return WFX::MwContinue;
})
```

!!! tip
    For a deeper understanding of how builtin coroutines work in WFX,
    see the **[Async](async.md)** page.