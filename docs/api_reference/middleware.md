# Middleware

Middleware in WFX provides a mechanism to intercept and control request processing **before a route handler may be invoked**, including the ability to short-circuit execution entirely.  
Typical use cases include authentication, authorization, logging, request preprocessing, and early rejection of requests.

Middleware can be registered globally or per-route. This page documents both **sync and async middleware**.

!!! important
    Middleware requires the user to always include the following header at the top of the file:
    ```cpp
    #include <http/middleware.hpp>
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

## Basic Middleware

Middleware must be registered before it can be used by any route.  
Registration is done using macros and follows the same deferred initialization model as routes.

**Example**:

```cpp
// Using a lambda
WFX_MIDDLEWARE("auth", [](Request& req, Response res) {
    /* ... */
    return MiddlewareAction::CONTINUE; // mandatory
});

// Using a function
MiddlewareAction AuthMiddleware(Request& req, Response res)
{
    /* ... */
    return MiddlewareAction::CONTINUE; // mandatory
}

WFX_MIDDLEWARE("auth", AuthMiddleware);
```

The above code:

- Registers a middleware under a string identifier.
- Registration occurs during static initialization and is finalized at engine startup.
- The name is used to define the execution order of middleware via the `[Project] middleware_list` section in `wfx.toml`.

!!! note
    Middleware registration and configuration follow these rules:

    1. If a middleware is defined in user code but **not listed** in `[Project] middleware_list`, it is treated as dead code and will never execute.  
       **No warning or error is currently emitted**. This is a known limitation and should be considered a bug.

    2. If a middleware name is listed in `[Project] middleware_list` but **no corresponding middleware is registered in user code**, the server will fail to start with a fatal error.

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
 *       -async machinery, including 'AsyncMiddlewareAction' and related types
 */
#include <async/builtins.hpp>

WFX_MIDDLEWARE("RequestCooldown", [](Request& _, Response res) -> AsyncMiddlewareAction {
    auto err = co_await Async::SleepFor(2000);

    if(err != Async::Status::NONE) {
        res.Status(HttpStatus::INTERNAL_SERVER_ERROR)
            .SendText("Middleware Failed to sleep for 2 seconds :(");

        co_return MiddlewareAction::BREAK;
    }

    co_return MiddlewareAction::CONTINUE;
})
```

!!! tip
    For a deeper understanding of how builtin coroutines work in WFX,
    see the **[Async](async.md)** page.