# Request & Response

Understanding how requests and responses work is fundamental to using WFX effectively. This page covers the types and methods you will interact with in user code.

---

## Request

`Request` represents an incoming HTTP request. It is an **opaque handle** backed by the engine. All data is accessed through API calls, ensuring strict control over memory, lifetime, and performance characteristics.

The request object is **valid only during the request lifecycle**. Any data returned (e.g., `std::string_view`) must not be retained beyond that scope unless copied.

Below are the primary members exposed by the `Request` structure.

- **`Method()`** - `HttpMethod`  
    Represents the HTTP method of the request (`GET`, `POST`, etc.).  
    **Read-only**, set by the engine.  

    ```cpp
    if(req.Method() == WFX::HttpMethod::GET) { /* handle GET */ }
    ```

- **`Version()`** - `HttpVersion`  
    HTTP version (`HTTP_1_0`, `HTTP_1_1`, `HTTP_2_0`, etc.).  
    **Read-only**, set by the engine.

    ```cpp
    if(req.Version() == WFX::HttpVersion::HTTP_1_1) { /* handle HTTP/1.1 */ }
    ```

- **`Path()`** - `std::string_view`  
    The requested path as a view into the request buffer.  
    Essentially **read-only**, but can be modified under controlled circumstances as long as you do not exceed the original size. Do not retain references beyond the request lifecycle.

    ```cpp
    if(req.Path() == "/login") { /* handle login */ }
    ```

- **`Body()`** - `std::string_view`  
    Raw request body. For POST/PUT requests, contains the payload.  
    Essentially **read-only**, but can be modified in place as long as you do not exceed the buffer size.

    ```cpp
    auto data = std::string(req.Body()); // copy if you need to keep it
    ```

- **`Headers`**  
    Represents HTTP headers. Headers are accessed via lookup functions. No direct container is exposed.

    ```cpp
    // 'GetHeader' returns true if header exists and writes value to 'ua'
    std::string_view ua;
    if(req.GetHeader("User-Agent", ua))
        printf("User-Agent: %.*s\n", (int)ua.size(), ua.data());
    ```

- **`Path Segments`**  
    Route paths can declare typed placeholders with `<name:type>` (the `name` is just for readability - it has no effect at runtime, only `type` is used). Four types are supported: `uint`, `int`, `uuid`, and `string`. A bare `*` as the last segment matches the rest of the path as a `string`.

    This lets route handlers read route parameters in a type-safe way without manual string parsing.

    **Example route**:  
    ```cpp
    WFX_GET("/users/<id:uint>/posts/<postId:int>", [](WFX::Request req, WFX::Response res) {
        /* ... */
    });
    ```

    **Incoming request**:  
    `/users/42/posts/100`

    **Conceptual internal representation**:
    ```cpp
    [
        uint64_t{42},
        int64_t{100}
    ]
    ```

    **Accessed via an index-based API** (segments are matched by position, not by the name written in the route - there is no name-based lookup):
    ```cpp
    // Returns number of parsed segments
    std::uint64_t segCount = req.SegmentCount();

    // Returns the typed segment at the given index
    auto segment = req.GetSegment(0); // Access 42

    // 'segment' is a variant type - pick the accessor matching the route's declared type
    segment.AsU64();   // uint
    segment.AsI64();   // int
    segment.AsString(); // string / *
    segment.AsUUID();   // uuid
    ```

    !!! warning
        Calling the wrong accessor for the segment's actual type (e.g. `AsU64()` on a `string` segment) is undefined behavior - match the accessor to the type declared in the route path. Calling `GetSegment()` with an out-of-range index is fatal.

    !!! note
        If a segment's value fails to parse as its declared type (e.g. a non-numeric value against `<id:uint>`), the route simply does not match and the request falls through to a normal 404 - it is never a runtime error.
    
- **`Context Store`**  
    Allows storing request-scoped values for the lifetime of the active request lifecycle.

    The context store is primarily intended for passing state between middleware, route handlers, and internal request-processing stages.

    Context values are type-aware and support both trivially copyable inline storage and dynamically allocated object storage depending on the stored type characteristics.

    **Store a value**:
    ```cpp
    req.SetContext<int>("user_id", 42);

    req.SetContext<std::string>(
        "session",
        "default_session"
    );
    ```

    **Retrieve a trivially copyable value**:
    ```cpp
    auto [id, ok] = req.GetContext<int>("user_id");

    if(ok)
        printf("User ID: %d\n", id);
    ```

    **Retrieve a non-trivial value**:
    ```cpp
    auto [session, ok] = req.GetContext<std::string>("session");

    if(ok && session)
        printf("Session: %s\n", session->c_str());
    ```

    **Erase a context value**:
    ```cpp
    req.EraseContext("session");
    ```

    !!! important
        `GetContext<T>()` behaves differently depending on the requested type category.

        For trivially copyable inline-stored types, it returns:

        ```cpp
        std::pair<T, bool>
        ```

        For dynamically stored non-trivial types, it returns:

        ```cpp
        std::pair<T*, bool>
        ```

        Type mismatches result in failed retrieval.

    !!! note
        Small trivially copyable types may be stored inline internally without heap allocation.

        Larger or non-trivial types are allocated through the engine memory allocator and automatically destroyed at request cleanup time.

    !!! tip
        The context store is intended for lightweight request-scoped coordination data.

        Each context operation may involve:

        - hash lookups,
        - string-key processing,
        - type metadata handling,
        - and possible dynamic allocation.

        Avoid storing large objects, high-frequency transient data, or performance-critical hot-path state inside the context store.

        Prefer compact, well-defined values genuinely needed across execution boundaries.

## Response

`Response` represents the outgoing HTTP response associated with the current request.

It provides a lightweight user-facing interface for:

- setting HTTP status codes,
- adding response headers,
- writing response bodies incrementally,
- sending files and templates,
- and registering streaming response generators.

`Response` itself does not own the underlying response state and acts only as a thin wrapper around engine-managed resources.
Internally, all operations are forwarded to the registered HTTP backend APIs.

Unlike traditional buffered HTTP abstractions, `Response` operates as a strict forward-only response builder with lifecycle-locked stages.

Response operations write into the engine-managed response pipeline buffers and metadata structures. Actual network transmission occurs later under backend control after route execution completes.

!!! note
    `Response` instances are only valid for the lifetime of the route execution scope managed by the engine.

    Users must not store, move across execution boundaries, or access `Response` objects outside their intended request lifecycle unless explicitly supported by the active execution model.

!!! danger
    `Response` enforces a strict ordered lifecycle model.

    The valid response progression is:

    ```text
    Status -> Header(s) -> Body Operations -> Commit (optional)
    ```

    Where body operations include:

    - `Write(...)`
    - `SendText(...)`
    - `SendFile(...)`
    - `SendTemplate(...)`
    - `Stream(...)`

    Each lifecycle stage permanently locks all previous stages.

    This means:

    - once headers are added, status modification becomes invalid,
    - once body operations begin, both status and headers become immutable,
    - once committed, the entire response becomes immutable.

    Invalid lifecycle transitions do not crash or terminate the engine. WFX logs an error, discards whatever was written so far, and forces the response to `500 Internal Server Error` with the message `Response contract violation`. Any further calls made on that same response are silently ignored.

    Invalid operation examples include:

    ```cpp
    res.Header("X-Test", "1");
    res.Status(HttpStatus::OK); // INVALID
    ```

    ```cpp
    res.Write("Hello");
    res.Header("Content-Type", "text/plain"); // INVALID
    ```

    ```cpp
    res.SendText("Hello");
    res.Write("More"); // INVALID
    ```

    ```cpp
    res.Commit();
    res.Commit(); // INVALID
    ```

Below are the primary methods exposed by `Response`.

- **`Status(HttpStatus code)`**  
  **`Status(std::uint16_t code)`**  
    Sets the HTTP status code for the response. Returns a reference to `Response` to allow chaining.

    If no explicit status is provided before body operations begin, the engine automatically defaults the response status to `200 OK`.

    !!! important
        Status may only be configured before headers or body operations begin.

        Once headers are added or body transmission starts, the response status becomes permanently locked.

    **Without chaining**:
    ```cpp
    res.Status(HttpStatus::OK);
    res.Header("Location", "/users/42");
    ```

    **With chaining**:
    ```cpp
    res.Status(HttpStatus::CREATED)
        .Header("Location", "/users/42");
    ```

- **`Header(std::string_view key, std::string_view value)`**  
    Adds an HTTP response header to the outgoing response metadata. Returns a reference to `Response` to allow chaining.

    The engine automatically appends all required protocol-level response headers internally, regardless of whether custom headers were provided by the user.

    !!! important
        Headers may only be added after status configuration and before body operations begin.

        Once the response body starts, all response metadata becomes permanently immutable.

    **Without chaining**:
    ```cpp
    res.Header("Content-Type", "application/json");
    res.Header("X-Powered-By", "WFX");
    ```

    **With chaining**:
    ```cpp
    res.Header("Content-Type", "application/json")
        .Header("X-Powered-By", "WFX");
    ```

- **`Write(...)`**  
    Writes response body data incrementally into the engine-managed response pipeline buffers.

    `Write()` supports multiple overloads for common primitive and utility types.

    **Supported overload categories**:

    ```cpp
    Write(std::string_view)
    Write(const char*)

    Write(const Shared::UUID&)

    Write(std::int64_t)
    Write(std::uint64_t)

    Write(std::int32_t)
    Write(std::uint32_t)

    Write(std::int16_t)
    Write(std::uint16_t)

    Write(std::int8_t)
    Write(std::uint8_t)

    Write(double)
    Write(float)

    Write(bool)
    ```

    Numeric values are internally formatted using stack-local conversion buffers without heap allocation.

    Multiple `Write()` calls may be chained freely during the body stage:

    ```cpp
    res.Write("User ID: ")
        .Write(42)
        .Write("\n")
        .Write("Premium: ")
        .Write(true);
    ```

- **`Commit()`**  
    Finalizes the active response pipeline.

    Internally, `Commit()` locks the response lifecycle completely and allows the backend to finalize protocol metadata such as content length handling and transmission state preparation.

    `Commit()` is primarily intended for manual `Write()` based response construction.

    **Example**:
    ```cpp
    res.Header("Content-Type", "text/plain");

    res.Write("Hello ")
        .Write("World ")
        .Write(42);

    res.Commit();
    ```

    !!! note
        Calling `Commit()` manually is optional in many cases.

        The engine may automatically finalize uncommitted responses internally when route execution completes.

        `Send*` functions already commit internally and do not require explicit manual commits.

- **`SendText(std::string_view data)`**  
    Convenience wrapper for sending plain text responses.

    Internally, `SendText()`:

    1. adds `Content-Type: text/plain`,
    2. writes the provided body into the response pipeline,
    3. and commits the response.

    Equivalent behavior:
    ```cpp
    res.Header("Content-Type", "text/plain")
        .Write(data)
        .Commit();
    ```

    **Example**:
    ```cpp
    res.SendText("Hello from WFX");
    ```

- **`SendFile(...)`**  
    Sends a file through the backend file transmission pipeline.

    **Overloads**:

    ```cpp
    SendFile(std::string_view path, bool autoHandle404 = true)
    SendFile(Shared::StringView path, bool autoHandle404 = true)
    ```

    Depending on the configured backend, optimized zero-copy transmission paths may be used internally.

    If `autoHandle404` is enabled and the target file does not exist, the engine automatically handles the failure response internally.

    The engine automatically appends all required protocol-level response headers internally, regardless of whether custom headers were provided by the user.

    **Example**:
    ```cpp
    res.SendFile("static/index.html");
    ```

    !!! important
        The provided path must be either:

        - an absolute path, or
        - a path relative to the engine working directory.

        Relative paths are resolved against the engine runtime location, not the caller source file or project root.

- **`SendTemplate(...)`**  
    Renders and sends a template response.

    **Overloads**:

    ```cpp
    SendTemplate(std::string_view path, Shared::JsonObject&& ctx)
    SendTemplate(Shared::StringView path, Shared::JsonObject&& ctx)
    ```

    The provided JSON context is forwarded to the template renderer for dynamic value binding.

    Internally, template rendering, body generation, and response finalization are handled entirely by the backend pipeline.

    The engine automatically appends all required protocol-level response headers internally, regardless of whether custom headers were provided by the user.

    **Example**:
    ```cpp
    auto o = WFX::RmJson();
    o["username"] = "atomic";
    o["id"] = 42;

    res.SendTemplate("profile.html", std::move(o));
    ```

    !!! important
        Template paths are resolved relative to the project template directory.

        Example:
        ```cpp
        res.SendTemplate("hello.html", ctx);
        ```

        resolves internally against:

        ```text
        templates/hello.html
        ```

    !!! tip
        For detailed information about template syntax, rendering behavior, and data binding,
        see the [**Templates**](templates.md) section.  
        For detailed information about json semantics, see the [**Json**](json.md) section.

- **`Stream(Fn&& fn, bool chunked = true)`**  
    Registers a streaming response generator with the backend.

    The provided callable is repeatedly invoked by the backend whenever additional response body data is required.

    This allows incremental generation of large or dynamically produced responses without buffering the full payload in memory.

    `Stream()` does not automatically add `Content-Type`.

    **Streaming callback signature**:
    ```cpp
    Shared::StreamResult(Shared::StreamBuffer)
    ```

    **Definitions**:
    ```cpp
    enum class StreamAction {
        CONTINUE,
        STOP_AND_ALIVE_CONN,
        STOP_AND_CLOSE_CONN
    };

    struct StreamResult {
        std::size_t  writtenBytes;
        StreamAction action;
    };

    struct StreamBuffer {
        char*       buffer;
        std::size_t size;
    };
    ```

    The provided callable is internally allocated through the engine memory allocator and remains owned by the backend until stream completion.

    !!! important
        Streaming callbacks may outlive the route callback that created them.

        Captured references must remain valid for the entire streaming lifetime.

        Capturing stack references is unsafe unless lifetime guarantees are externally enforced.

    **Example**:
    ```cpp
    res.Header("Content-Type", "application/octet-stream");

    res.Stream([
        offset = std::size_t{0}
    ](Shared::StreamBuffer buffer) mutable -> Shared::StreamResult {

        std::size_t bytes = ReadFromSource(offset, buffer.buffer, buffer.size);

        if(bytes == 0) {
            return {
                0,
                Shared::StreamAction::STOP_AND_ALIVE_CONN
            };
        }

        offset += bytes;

        return {
            bytes,
            Shared::StreamAction::CONTINUE
        };
    }, false);
    ```

    !!! note
        Stream buffer capacity is controlled by the `[Network] send_buffer_max` configuration value in `wfx.toml`.