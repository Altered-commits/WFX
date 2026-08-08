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

- `path` - the route path as a string literal. Supports typed dynamic segments (e.g. `/users/<id:uint>`) - see the [Request & Response](request_and_response.md) page for the full syntax and how to read them.
- `handler` - a callable object or lambda with signature **`void(WFX::Request, WFX::Response)`**.

---

## HTTP Methods

Every HTTP method has its own macro, all with an identical signature:

```cpp
WFX_GET("/users/<id:uint>", ...)
WFX_POST("/users", ...)
WFX_PUT("/users/<id:uint>", ...)
WFX_PATCH("/users/<id:uint>", ...)
WFX_DELETE("/users/<id:uint>", ...)
WFX_HEAD("/users/<id:uint>", ...)
WFX_OPTIONS("/users/<id:uint>", ...)
```

Each has an `_EX` variant for attaching middleware (`WFX_PUT_EX`, `WFX_PATCH_EX`, etc.), the same
way `WFX_GET_EX` works, see [Routes with Middleware](#routes-with-middleware) below.

Two of these methods have engine-level behavior worth knowing before you reach for them:

- `WFX_HEAD` - WFX does **not** automatically fall back to a matching `WFX_GET` handler for a path
  with no `WFX_HEAD` registered. A `HEAD` request to a `GET`-only route is a plain `404` today, not
  an automatic bodyless `GET`. Register `WFX_HEAD` explicitly wherever you need it.
- `WFX_OPTIONS` - see [CORS and OPTIONS](#cors-and-options) directly below. A registered
  `WFX_OPTIONS` handler only runs for requests the engine doesn't already answer for you, which is
  most `OPTIONS` traffic in practice.

---

## CORS and OPTIONS

WFX has Cross-Origin Resource Sharing built into the engine itself, turned on with a `[CORS]`
section in `wfx.toml`, see [WFX Settings](../core_concepts/wfx_toml.md#cors) for every field.
Nothing shown on this page requires writing a `WFX_OPTIONS` route, this section explains what the
engine does on its own and where a route you register fits around it.

There are two independent mechanisms, and knowing which one answers a given `OPTIONS` request
matters for reasoning about your own routes:

**CORS preflight** - a real preflight is an `OPTIONS` request carrying both an `Origin` header and
an `Access-Control-Request-Method` header (only a browser sends this second header, never a
regular client). When `[CORS]` is enabled and the `Origin` matches `allowed_origins`, the engine
answers the preflight completely on its own, before routing and before any middleware, though it
still goes through connection/rate limiting first like any other request, a preflight is just as
cheap to mass-produce as any other request and isn't exempt from that. Past the limiter, this
happens even for a path with no route registered at all, a preflight validates the origin's intent
to make a cross-origin request, it says nothing about whether the real request that follows will
find anything.

!!! danger "A registered `WFX_OPTIONS` route does not see a real preflight"
    If a preflight's origin matches, the engine answers it and returns immediately, your own
    `WFX_OPTIONS` handler at that same path, if you registered one, never runs. There's no way to
    override this per-route today, `[CORS]` is all-or-nothing for the process.

**Generic `OPTIONS` fallback** - every other `OPTIONS` request, no `Origin` header, an `Origin`
that doesn't match, or CORS disabled entirely, falls through to normal routing like any other
method. If you registered a `WFX_OPTIONS` handler at that path, it runs normally, same as any
other route. If you didn't, and the path has a real route under at least one other method, the
engine auto-answers with `204 No Content` and an `Allow:` header listing every method actually
registered there, e.g. `Allow: GET, POST, PUT`. If the path has no route under any method, it's a
plain `404`, same as it would be for any other unmatched method.

!!! note "The two Allow-Methods lists are unrelated"
    A CORS preflight's `Access-Control-Allow-Methods` header always comes from `[CORS]`'s static
    `allowed_methods` config, the exact same string on every path. The generic fallback's `Allow:`
    header is the opposite, built per-path from whatever methods you actually registered there. A
    path with `WFX_GET`/`WFX_POST` only will preflight-answer with the full configured method list,
    but generic-fallback-answer with just `Allow: GET, POST`. Neither one validates the other.

**Summary**, in the order these checks actually run:

1. Connection/rate limiting, same as every other request, `OPTIONS` included.
2. Real preflight, origin matches -> answered by the engine, your routes never see it.
3. Everything else (`OPTIONS` with no marker, no `Origin`, or an unmatched `Origin`) -> normal
   routing. A registered `WFX_OPTIONS` handler runs.
4. No `WFX_OPTIONS` handler, but the path exists under other methods -> auto `204` + `Allow:`.
5. No route under any method -> `404`.

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

- Middleware must be provided via `WFX_MW_LIST`, using the `_EX` variant of the route macro (`WFX_GET_EX`, `WFX_POST_EX`).  
- Even if the route uses only a single middleware function, it must still be wrapped in `WFX_MW_LIST`.  

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
```

---

## Async Routes

WFX routes can be declared **async** by returning an async task type (e.g. `WFX::Coro`).
This allows the route handler itself to `co_await` builtins such as `SleepFor`, database calls, or other async operations.

The signature is identical to a normal route, except the lambda returns an async coroutine type:

```cpp
/*
 * NOTE: This header is mandatory when using any builtin async utilities such as
 * functions like 'SleepFor'. It also brings in the core async machinery,
 * including 'WFX::Coro' and related types.
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