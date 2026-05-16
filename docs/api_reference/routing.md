# Routing

WFX provides a **macro-based routing system** that allows registering request handlers in a declarative manner.  
Routes are automatically registered during program initialization through deferred execution, ensuring deterministic order and avoiding manual registration.  

This page covers both **sync and async** routes.

!!! important
    Routing requires the user to always include the routing header at the top of the file:
    ```cpp
    #include <wfx/http.hpp>
    ```

!!! danger
    - The `path` argument in route and group macros must refer to a string that remains valid for the lifetime of the program.  
    Using a temporary or short-lived string (e.g., a locally created `std::string`) will cause undefined behavior and may crash the server.

    - Routes defined by the developer are not fully validated by WFX.  
    While **incoming request paths are normalized and checked thoroughly**, WFX does **not** validate the route definitions themselves.  
    Defining unsafe or nonsensical paths (e.g., containing `../..`) can lead to undefined behavior.  
    Ensure all route paths are correctly and safely specified.

---

## Basic Routes

Routes are defined using method-specific macros:

```cpp
WFX_GET("/health", [](WFX::Request req, WFX::Response res) {
    res.SendText("OK");
});

WFX_POST("/login", [](WFX::Request req, WFX::Response res) {
    // Handle login request
});
```

`WFX_GET(path, handler)` / `WFX_POST(path, handler)` - macros corresponding to HTTP methods.

- `path` - the route path as a string literal. Supports dynamic segments (see below).
- `handler` - a callable object or lambda with signature **`void(WFX::Request, WFX::Response)`**.

---

## Route Groups

Route groups allow applying a common prefix to multiple routes:

```cpp
WFX_GROUP_START("/api")

    WFX_GET("/users", [](WFX::Request req, WFX::Response res) {
        // List users
    });

    WFX_POST("/users", [](WFX::Request req, WFX::Response res) {
        // Create user
    });

WFX_GROUP_END()

// Now the routes for GET and POST become /api/users
```

`WFX_GROUP_START(path)` - pushes a prefix (`path`) to all routes within the group.  
`WFX_GROUP_END()` - pops the last pushed prefix.

Groups may be nested to form hierarchical route structures.

!!! warning
    Each `WFX_GROUP_START` must be paired with a corresponding `WFX_GROUP_END`.  
    If the number of start and end macros does not match, the server will fail to start and terminate with an error.

---

## Routes with Middleware

WFX allows routes to execute **middleware** before the main handler.  
Middleware can perform tasks such as authentication, logging, or input validation.
Each route can have its own middleware stack, which is executed in order before the route handler is called.

**Key Points**:

- Middleware must be provided either via `WFX_MW_LIST` or using `MakeMiddlewareFromFunctions`.  
- Even if the route uses only a single middleware function, it must be wrapped with one of these helpers.  

**Example**:

```cpp
#include <wfx/http.hpp>

// 'AuthMiddleware' and 'SecurityMiddleware' is applied only to this route
// It does not affect other routes
WFX_GET_EX(
    "/secure",
    WFX_MW_LIST(AuthMiddleware, SecurityMiddleware, ...),
    [](WFX::Request req, WFX::Response res) { 
        res.SendText("Protected content"); 
    }
);

// Or

WFX_GET_EX(
    "/secure",
    MakeMiddlewareFromFunctions(AuthMiddleware, SecurityMiddleware, ...),
    [](WFX::Request req, WFX::Response res) { 
        res.SendText("Protected content"); 
    }
);
```

---

## Async Routes

WFX routes can be declared **async** by returning an async task type (e.g. `WFX::Coro`).
This allows the route handler itself to `co_await` builtins such as `SleepFor`, database calls, or other async operations.

The signature is identical to a normal route, except the lambda returns an async coroutine type:

```cpp
/*
 * NOTE: This header is mandatory when using any builtin async utilities-
 *       -such as functions like 'SleepFor'. It also brings in the core-
 *       -async machinery, including 'WFX::Coro' and related types
 */
#include <wfx/async.hpp>
#include <wfx/http.hpp>

WFX_GET("/async", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    auto err = co_await WFX::SleepFor(2000);

    if(err != WFX::AsyncOk)
        res.Status(WFX::HttpStatus::INTERNAL_SERVER_ERROR)
            .SendText("Route failed to sleep for 2 seconds :(");
    else
        res.SendText("Ok");
});
```

!!! tip
    For a deeper understanding of how builtin coroutines work in WFX,
    see the **[Async](async.md)** page.