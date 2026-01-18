# Request & Response

Understanding how requests and responses work is fundamental to using WFX effectively. This page covers the types and methods you will interact with in user code.

---

## Request

`Request` represents an incoming HTTP request. It contains all data associated with the request lifecycle, including metadata, headers, body, and parsed path information. It also exposes a per-request context store that can be used to share data between middleware, routes, and user code.

!!! note
    Although the underlying type is `HttpRequest`, it is internally aliased to `Request` for user-facing APIs.  
    This is done intentionally to mirror `Response`, which is also a user-side abstraction, and to keep request and response usage consistent in user code.

Below are the primary members exposed by the `Request` structure.

- **`method`** - `HttpMethod`  
    Represents the HTTP method of the request (`GET`, `POST`, etc.).  
    **Read-only**, set by the engine.  

    ```cpp
    if(req.method == HttpMethod::GET) { /* handle GET */ }
    ```

- **`version`** - `HttpVersion`  
    HTTP version (`HTTP_1_0`, `HTTP_1_1`, `HTTP_2_0`, etc.).  
    **Read-only**, set by the engine.

    ```cpp
    if(req.version == HttpVersion::HTTP_1_1) { /* handle HTTP/1.1 */ }
    ```

- **`path`** - `std::string_view`  
    The requested path as a view into the request buffer.  
    Essentially **read-only**, but can be modified under controlled circumstances as long as you do not exceed the original size. Do not retain references beyond the request lifecycle.

    ```cpp
    if(req.path == "/login") { /* handle login */ }
    ```

- **`body`** - `std::string_view`
    Raw request body. For POST/PUT requests, contains the payload.  
    Essentially **read-only**, but can be modified in place as long as you do not exceed the buffer size.

    ```cpp
    auto data = std::string(req.body); // copy if you need to keep it
    ```

- **`headers`** - `std::unordered_map<std::string_view, std::string_view>`  
    Represents HTTP headers. Provides lookup and iteration.

    ```cpp
    // 'GetHeader' returns an empty std::string_view if not found
    auto ua = req.headers.GetHeader("User-Agent");
    printf("User-Agent: %.*s\n", (int)ua.size(), ua.data());

    // You can also use 'CheckAndGetHeader' to get a pair {exists?, value}
    auto [exists, token] = req.headers.CheckAndGetHeader("X-Token");
    if(!exists) { /* handle error */ }
    else        { /* handle token */ }

    // Or you can set header, unlikely but possible via 'SetHeader'
    req.headers.SetHeader("My-Value", "WFX");
    ```

- **`pathSegments`** - `std::vector<std::variant<...>>`  
    Contains the parsed components of the request path. Each segment represents either a literal path component or a typed route parameter extracted from the URL (for example integers or UUIDs).

    This allows route handlers to access route parameters in a type-safe manner without performing manual string parsing or conversions.

    **Example route**:  
    ```cpp
    WFX_GET("/users/<int>/posts/<uint>", [](Request& req, Response& res) {
        /* ... */
    });
    ```

    **Incoming request**:  
    `/users/42/posts/100`

    **Conceptual internal representation**:
    ```cpp
    [
        int64_t{42},
        uint64_t{100}
    ]
    ```

    **Accessing values manually**:
    ```cpp
    auto userId = std::get<int64_t>(req.pathSegments[0]);
    auto postId = std::get<uint64_t>(req.pathSegments[1]);
    ```

    **Accessing values using segment macros**:
    ```cpp
    auto userId = GetSegmentAsInt(req.pathSegments[0]);
    auto postId = GetSegmentAsUInt(req.pathSegments[1]);
    ```

    !!! note
        The purpose and usage of `pathSegments` will become much clearer in the Routing section.
    
- **`context`** - `std::unordered_map<std::string, std::any>`
    Allows storing arbitrary values for the lifetime of the request. It is useful for passing data between middleware and route handlers.

    ```cpp
    // Store a value
    req.SetContext<int>("user_id", 42);

    // Retrieve a value (returns pointer to value if it exists, else nullptr)
    if(auto id = req.GetContext<int>("user_id")) {
        printf("User ID: %d\n", *id);
    }

    // Initialize or get value
    auto* ptr = req.InitOrGetContext<std::string>("session", "default_session");
    ```

    !!! tip
        While the context map is flexible, it is not cheap. Each entry involves a hash lookup, a std::string key, and a std::any allocation. Avoid storing large objects or excessive transient data in the context. Prefer storing small, well-defined values that are genuinely needed across middleware and handlers. Overusing the context can negatively impact performance and cache locality.

## Response

`Response` represents the outgoing HTTP response. It is the primary interface used by application code to control status codes, headers, and the response body sent back to the client.

It is a **user-facing abstraction**: internally, all operations are forwarded to the engine via provided APIs. Users never interact directly with the underlying response implementation.

!!! note
    `Response` does not own the underlying response object and internally holds pointers to engine-managed state.

    In **synchronous routes**, the `Response` instance is guaranteed to be valid for the entire duration of the user callback.

    In **asynchronous routes**, this guarantee no longer holds once execution yields. If an async operation needs to continue using the response after yielding, the response state **must be explicitly copied**. This is required because the underlying HTTP response object is only guaranteed to exist while the connection is alive and under engine control.

    This pattern is **not recommended** and should only be used when absolutely necessary. Prefer designing async logic such that the response is finalized within the intended execution scope.

!!! danger
    **All send and stream operations are single-use per request–response lifecycle.**

    Any `Send*` or `Stream*` function **must be called exactly once** during a single request–response cycle.  
    Calling any send or stream function again within the same cycle is **fatal** and will **terminate the engine**.

    After the request–response lifecycle completes, if the connection is still alive, the engine will automatically reset its internal state, allowing send/stream operations to be used again for the next cycle.

Below are the primary methods exposed by `Response`.

- **`Status(HttpStatus code)`**  
    Sets the HTTP status code for the response.

    Returns a reference to `Response` to allow chaining.

    **Without chaining**:
    ```cpp
    res.Status(HttpStatus::OK);
    res.Set("Location", "/users/42");
    ```

    **With chaining**:
    ```cpp
    res.Status(HttpStatus::CREATED)
        .Set("Location", "/users/42");
    ```

- **`Set(std::string key, std::string value)`**  
    Sets or overrides an HTTP response header.
    Both key and value are moved into the response. Header names are treated as-is.  
    
    Returns a reference to `Response` to allow chaining.

    **Without chaining**:
    ```cpp
    res.Set("Content-Type", "application/json");
    res.Set("X-Powered-By", "WFX");
    ```

    **With chaining**:
    ```cpp
    res.Set("Content-Type", "application/json")
        .Set("X-Powered-By", "WFX");
    ```

- **`SendText(...)`**  
    Sends a plain text response body. The appropriate content type (`text/plain`) is set internally.

    **Overloads**:

    - `SendText(const char* cstr)`
    - `SendText(std::string&& str)`

    **Example**:
    ```cpp
    // Uses const char* overload
    res.SendText("Hello from WFX");

    // Uses std::string overload
    res.SendText(std::string("Dynamic response"));
    ```

- **`SendJson(const Json& j)`**  
    Sends a JSON response. The appropriate content type (`application/json`) is set internally.

    **Example**:
    ```cpp
    res.SendJson(Json::object({
        {"status", "ok"},
        {"value", 42}
    }));
    ```

- **`SendFile(...)`**  
    Sends a file from disk as the response body. The appropriate content type is handled internally (**based on file extension**).

    **Overloads**:

    - `SendFile(const char* path, bool autoHandle404 = true)`
    - `SendFile(std::string&& path, bool autoHandle404 = true)`

    If `autoHandle404` is enabled and the file does not exist, the engine will automatically handle the error response.  
    If `autoHandle404` is disabled and the file does not exist, the networking backend will handle the situation as a fallback. However, this is not recommended, as it is intended only to prevent **undefined behavior or server crashes**, not as a **primary error-handling mechanism**.

    **Example**:
    ```cpp
    // Uses const char* overload
    res.SendFile("static/index.html");
    ```

    !!! important
        The provided path must be either:

        - an absolute path, or
        - a path **relative to the engine's working directory**.

        Relative paths are resolved against the engine location itself, **not** the caller's source file or project root.

- **`SendTemplate(...)`**  
    Renders and sends a template. The appropriate content type (`text/html`) is set internally.

    **Overloads**:

    - `SendTemplate(const char* path, Json&& ctx = {})`
    - `SendTemplate(std::string&& path, Json&& ctx = {})`

    The optional JSON context is provided to the template renderer to populate dynamic content.  
    If the specified template cannot be found, the engine will automatically send a **404 Template Not Found** response.

    **Example**:
    ```cpp
    // Uses const char* overload + no JSON context provided (static template)
    res.SendTemplate("index.html");

    // Uses const char* overload + JSON context provied (dynamic template)
    res.SendTemplate("profile.html", Json::object({
        {"username", "atomic"},
        {"id", 42}
    }));
    ```

    !!! tip
        For detailed information about template syntax, compilation, rendering modes, and data binding,
        see the [**Templates**](templates.md) section.

- **`Stream(StreamGenerator generator, bool streamChunked = true)`**  
    Initiates a streaming response. The provided generator is repeatedly called by the networking backend whenever it is ready to accept more data. This allows incremental or large responses to be sent without buffering the entire payload in memory.

    **Important**: `Stream` does not set `Content-Type` header internally.

    **Definitions**:
    ```cpp
    enum class StreamAction {
        CONTINUE,             // Continue streaming
        STOP_AND_ALIVE_CONN,  // Stop streaming and keep the connection alive
        STOP_AND_CLOSE_CONN   // Stop streaming and close the connection
    };

    struct StreamResult {
        std::size_t  writtenBytes; // Number of bytes written into buffer
        StreamAction action;       // Action to take after this invocation
    };

    struct StreamBuffer {
        char*       buffer; // Writable buffer provided by the engine
        std::size_t size;   // Size of the buffer in bytes
    };

    // Streaming generator signature
    using StreamGenerator = MoveOnlyFunction<StreamResult(StreamBuffer)>;
    ```

    **Example**:
    ```cpp
    // Streaming a file-like source
    res.Stream([
        offset = std::size_t{0}
    ](StreamBuffer buffer) mutable {
        std::size_t bytes = ReadFromSource(offset, buffer.buffer, buffer.size);
        
        // End of data
        // Stop streaming and keep the connection alive
        if(bytes == 0) {
            return StreamResult{
                0,
                StreamAction::STOP_AND_ALIVE_CONN
            };
        }

        // Continue streaming for more data
        offset += bytes;
        return StreamResult{
            bytes,
            StreamAction::CONTINUE
        };
    }, false);
    ```

    !!! note
        - Streaming generators are executed under the engine's control. They are invoked only when the networking backend is ready to send data. This model avoids buffering entire responses in memory and provides explicit control over connection lifetime.
        - Buffer capacity is controlled by the `[Network] send_buffer_max` value in `wfx.toml`.