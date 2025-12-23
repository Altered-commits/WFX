# Routing

WFX provides a **macro-based routing system** that allows registering request handlers in a declarative manner.  
Routes are automatically registered during program initialization through deferred execution, ensuring deterministic order and avoiding manual registration.  

Routes can be **sync or async**, but this page only covers **sync** routes.

!!! important
    Routing requires the user to always include the routing header at the top of the file:
    ```cpp
    #include <http/routes.hpp>
    ```

!!! danger
    - The `path` argument in route and group macros must refer to a string that remains valid for the lifetime of the program.  
    Using a temporary or short-lived string (e.g., a locally created `std::string`) will cause undefined behavior and may crash the server.

    - Routes defined by the developer are not fully validated by WFX.  
    While incoming request paths are normalized and checked thoroughly, WFX does **not** validate the route definitions themselves.  
    Defining unsafe or nonsensical paths (e.g., containing `../..`) can lead to undefined behavior.  
    Ensure all route paths are correctly and safely specified.

---

## Basic Routes

Routes are defined using method-specific macros:

```cpp
WFX_GET("/health", [](Request& req, Response& res) {
    res.SendText("OK");
});

WFX_POST("/login", [](Request& req, Response& res) {
    // Handle login request
});
```

`WFX_GET(path, handler)` / `WFX_POST(path, handler)` - macros corresponding to HTTP methods.

- `path` - the route path as a string literal. Supports dynamic segments (see below).
- `handler` - a callable object or lambda with signature **`void(Request&, Response&)`**.

---

## Route Groups

Route groups allow applying a common prefix to multiple routes:

```cpp
WFX_GROUP_START("/api")

    WFX_GET("/users", [](Request& req, Response& res) {
        // List users
    });

    WFX_POST("/users", [](Request& req, Response& res) {
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

## Dynamic Path Segments

Routes can include dynamic segments that extract values from the URL. A segment can optionally have a name before the colon (`:`) for readability, but the name is not required. The engine only uses the segment type for parsing and indexing.

**Helper Macro**:

WFX provides helper macros to simplify access to dynamic path segments, but all of these operations can also be done manually if needed. The macros are essentially shortcuts for extracting and converting segments.

| Macro                         | Equivalent Manual Access              |
|-------------------------------|---------------------------------------|
| `GetSegmentAsString(segment)` | `std::get<std::string_view>(segment)` |
| `GetSegmentAsInt(segment)`    | `std::get<int64_t>(segment)`          |
| `GetSegmentAsUInt(segment)`   | `std::get<uint64_t>(segment)`         |
| `GetSegmentAsUUID(segment)`   | `std::get<WFX::Utils::UUID>(segment)` |

**Segment Indexing**:

Segments are indexed in the order they appear, starting from 0.

Example with multiple segments:

```cpp
WFX_GET("/user/<id:int>/posts/<pid:int>", [](Request& req, Response& res) {
    int64_t userId = GetSegmentAsInt(req.pathSegments[0]);
    int64_t postId = GetSegmentAsInt(req.pathSegments[1]);
});
```

!!! danger "Note"
    - Out-of-bounds access is not checked by WFX. Attempting to read a segment index that does not exist will cause undefined behavior. Developers are responsible for ensuring segment indices are correct.
    - Accessing a `/<int>` path segment using an incompatible type will cause `std::get<>` to throw an exception.
    Always extract segments using the correct helper for their declared type (e.g., do not read an `int` segment as `uint` or `UUID`).  
    Failing to do so results in unnecessary exceptions and, if unhandled, may crash the server.

**Supported Segment Types**:

| Segment Type | Helper Macro	               | Return Type      |
|--------------|-------------------------------|------------------|
| int	       | `GetSegmentAsInt(segment)`	   | int64_t          |
| uint	       | `GetSegmentAsUInt(segment)`   | uint64_t         |
| string	   | `GetSegmentAsString(segment)` | std::string_view |
| uuid	       | `GetSegmentAsUUID(segment)`   | WFX::Utils::UUID |

**Example**:
```cpp
// Named segment
WFX_GET("/user/<id:int>", [](Request& req, Response& res) {
    int64_t userId = GetSegmentAsInt(req.pathSegments[0]);
});

// Unnamed segment (also valid) + manual access
WFX_GET("/user/<int>", [](Request& req, Response& res) {
    int64_t userId = std::get<int64_t>(req.pathSegments[0]);
});
```

- `<id:int>` - id is optional; used as a comment for developer understanding.
- `<int>` - valid, same as above but without a name.

---

## Routes with Middleware

WFX allows routes to execute **middleware** before the main handler.  
Middleware can perform tasks such as authentication, logging, or input validation. Each route can have its own middleware stack, which is executed in order before the route handler is called.

**Key Points**:

- Middleware must be provided either via `WFX_MW_LIST` or using `MakeMiddlewareFromFunctions`.  
- Even if the route uses only a single middleware function, it must be wrapped with one of these helpers.  
- The middleware system requires including `<http/middleware.hpp>` in your source file.

**Example**:

```cpp
#include <http/middleware.hpp>

// 'AuthMiddleware' and 'SecurityMiddleware' is applied only to this route
// It does not affect other routes
WFX_GET_EX(
    "/secure",
    WFX_MW_LIST(AuthMiddleware, SecurityMiddleware, ...),
    [](Request& req, Response& res) { 
        res.SendText("Protected content"); 
    }
);

// Or

WFX_GET_EX(
    "/secure",
    MakeMiddlewareFromFunctions(AuthMiddleware, SecurityMiddleware, ...),
    [](Request& req, Response& res) { 
        res.SendText("Protected content"); 
    }
);
```