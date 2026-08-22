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

- **`Query Params`**  
    `Path()` keeps the raw `?...` suffix exactly as it arrived, so `GetQueryParams()` is what actually reads it. It parses `key=value&key=value` once into a `QueryParams` object, then `Get()` looks a key up against that parsed set.

    ```cpp
    WFX_GET("/search", [](WFX::Request req, WFX::Response res) {
        auto qp = req.GetQueryParams();

        std::string_view q;
        if(!qp.Get("q", q)) {
            res.Status(WFX::HttpStatus::BAD_REQUEST).SendText("missing q");
            return;
        }

        res.SendText(q);
    });
    ```

    For `GET /search?q=hello&page=2`, `qp.Get("q", q)` returns `true` and sets `q` to `"hello"`.

    `Count()` returns how many pairs were parsed:

    ```cpp
    std::uint64_t total = qp.Count();
    ```

    !!! important
        - `Get()` returns the raw value exactly as it appeared on the wire. It is not percent-decoded, the same way `GetHeader()` does not decode header values either. If a client sends `?email=a%40b.com`, `Get("email", out)` gives you `"a%40b.com"`, not `"a@b.com"`, decode it yourself if you need to.
        - If a key appears more than once (`?v=first&v=second`), `Get()` returns the first one it finds.

    !!! tip
        `GetQueryParams()` parses the whole query string every time it's called. If a handler reads several keys, call it once and reuse the result rather than calling it once per key.

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
- registering streaming response generators,
- and awaitable outbound chunk flushing.

`Response` itself does not own the underlying response state and acts only as a thin wrapper around engine-managed resources.
Internally, all operations are forwarded to the registered HTTP backend APIs.

Unlike traditional buffered HTTP abstractions, `Response` operates as a strict forward-only response builder with lifecycle-locked stages.

Response operations write into the engine-managed response pipeline buffers and metadata structures. Actual network transmission occurs later under backend control after route execution completes.

!!! danger
    `Response` enforces an ordered lifecycle, but it is not one single chain. There are two
    independent rules, and they are easy to mix up:

    - **`Status(...)` and `Header(...)` are order-free with each other.** Either can come first,
      and `Status(...)` can be called more than once, whichever call happens last before the body
      starts is the one that takes effect.
    - **`PersistentHeader(...)` must come before the *first* `Header(...)` call.** It composes
      freely with `Status(...)` in either order, but the moment any `Header(...)` call happens,
      every later `PersistentHeader(...)` call on that same response is a contract violation, no
      matter how many more `Status(...)`/`Header(...)` calls follow it.

    Both rules stop applying once a body operation starts, which is where the real forward-only
    lock begins:

    ```text
    [ Status(...) / Header(...) / PersistentHeader(...) ] -> Body Operations -> Commit (optional)
    ```

    Where body operations include:

    - `Write(...)`
    - `SendText(...)`
    - `SendFile(...)`
    - `SendTemplate(...)`
    - `Stream(...)`
    - `FlushStart(...)`

    Once a body operation begins, status and every kind of header become immutable. Once
    committed, the entire response becomes immutable.

    Invalid lifecycle transitions do not crash or terminate the engine. WFX logs an error, discards whatever was written so far, and forces the response to `500 Internal Server Error` with the message `Response contract violation`. Any further calls made on that same response are silently ignored.

    Invalid operation examples include:

    ```cpp
    res.Header("Content-Type", "text/plain");
    res.PersistentHeader("Access-Control-Allow-Origin", "*"); // INVALID: a Header() already ran
    ```

    ```cpp
    res.Write("Hello");
    res.Status(HttpStatus::OK); // INVALID: status after a body operation
    ```

    ```cpp
    res.Write("Hello");
    res.Header("Content-Type", "text/plain"); // INVALID: header after a body operation
    ```

    ```cpp
    res.SendText("Hello");
    res.Write("More"); // INVALID: write after commit
    ```

    ```cpp
    res.Commit();
    res.Commit(); // INVALID
    ```

    ```cpp
    res.Write("Hello");
    res.FlushStart(); // INVALID: FlushStart() must be the very first body operation
    ```

!!! important "Status line has no reason phrase"
    WFX sends `HTTP/1.1 200 ` rather than `HTTP/1.1 200 OK`. The reason phrase is optional per
    RFC 7230 (clients are told to ignore it, and HTTP/2 has no reason phrase at all), and dropping
    it is what makes the status code patchable in place at a fixed byte offset, which is what lets
    `Status(...)` be called more than once instead of only as the very first call on the response.

Below are the primary methods exposed by `Response`.

- **`Status(HttpStatus code)`**  
  **`Status(std::uint16_t code)`**  
    Sets the HTTP status code for the response. Returns a reference to `Response` to allow chaining.

    If no explicit status is provided before body operations begin, the engine automatically defaults the response status to `200`.

    !!! important
        `Status(...)` can be called at any point before a body operation starts, in any order
        relative to `Header(...)` and `PersistentHeader(...)` calls, and more than once, the last
        call before the body starts is the one that wins.

        Once a body operation begins, the status becomes permanently locked.

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

    **Headers before status is fine too**:
    ```cpp
    res.Header("X-Request-Id", requestId); // the eventual status isn't known yet
    // ... later, once the real outcome is known:
    res.Status(HttpStatus::NOT_FOUND).SendText("not found");
    ```

- **`Header(std::string_view key, std::string_view value)`**  
    Adds an HTTP response header to the outgoing response metadata. Returns a reference to `Response` to allow chaining.

    The engine automatically appends all required protocol-level response headers internally, regardless of whether custom headers were provided by the user.

    !!! important
        `Header(...)` can be called before, after, or interleaved with `Status(...)` calls, in any
        order, as long as no body operation has started yet.

        The first `Header(...)` call closes the window for `PersistentHeader(...)`, see below.

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

- **`PersistentHeader(std::string_view key, std::string_view value)`**  
    Adds a header the same way `Header(...)` does, but built for code that runs before the route
    handler, most commonly middleware, and that has no idea yet what the eventual status will be.

    It differs from `Header(...)` in one behavior that matters specifically for error paths: if
    something later forces the response into an error rebuild (a contract violation, `SendFile`'s
    automatic `404` for a missing file, a missing template), a `Header(...)` call gets discarded
    along with everything else that was written. A `PersistentHeader(...)` call survives that
    rebuild and still shows up on the response that actually gets sent.

    This exists for headers that need to be on *every* response regardless of outcome, CORS
    headers being the motivating case: middleware has to add `Access-Control-Allow-Origin` before
    the route handler has run, and it still needs to be there even if the request ends in a `404`
    or a `500`.

    !!! important
        `PersistentHeader(...)` must be called before the *first* `Header(...)` call on the
        response. It can be called before or after `Status(...)` freely, but once any
        `Header(...)` call has happened, every later `PersistentHeader(...)` call is a contract
        violation, regardless of how many more `Status(...)`/`Header(...)` calls follow it.

    **Example (middleware setting a header the handler doesn't know about yet)**:
    ```cpp
    WFX_MIDDLEWARE("cors", [](WFX::Request req, WFX::Response res) {
        res.PersistentHeader("Access-Control-Allow-Origin", "https://example.com");
        return WFX::MwContinue;
    });

    // Whichever status the handler ends up picking, the header above is still on the response,
    // even if this route doesn't exist and the request ends in a 404
    WFX_GET("/users/<id:uint>", [](WFX::Request req, WFX::Response res) {
        res.Status(HttpStatus::NOT_FOUND).SendText("no such user");
    });
    ```

    !!! tip "WFX has built-in CORS support"
        The example above is illustrative of what `PersistentHeader(...)` is for, hand-writing CORS
        headers in your own middleware like this. In practice you don't need to: WFX has CORS
        built into the engine itself, origin allowlisting, preflight, credentials, all of it,
        turned on with a `[CORS]` section in `wfx.toml`. See [Routing](routing.md#cors-and-options)
        for how it behaves and [WFX Settings](../core_concepts/wfx_toml.md#cors) for the config.

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

- **`FlushStart()`**  
    Opens the response body in awaitable outbound-flush mode. `Transfer-Encoding: chunked` is sent
    right away, and every `Write(...)` call from here on only stages data locally until an
    explicit `Flush()` actually sends it.

    This is the awaitable counterpart to `Stream(...)`. Both send chunked data incrementally, but
    `Stream(...)` hands the engine a generator callback that it drives *after* the route handler
    has already returned, while `Flush()`/`FlushEnd()` send real bytes *from inside* the handler
    coroutine, so a handler can freely `co_await` something else, an upstream fetch, a database
    row batch, between rounds. See [Async](async.md) for `co_await`/`WFX::Coro` basics.

    !!! important
        `FlushStart()` must be the very first body operation, before any `Write(...)` call.
        Calling it after the body has already started some other way is a contract violation:
        `Content-Length` and `Transfer-Encoding: chunked` are mutually exclusive response framings,
        and that choice has to be made before the first body byte goes out, there is no way to
        insert a header retroactively without moving already-written body bytes.

    **Example**:
    ```cpp
    WFX_GET("/export.csv", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
        res.Header("Content-Type", "text/csv");
        res.FlushStart();

        res.Write("id,name\r\n");
        // ... more Write(...) + Flush()/FlushEnd() below
        co_return;
    });
    ```

- **`Flush()`** *(awaitable)*  
    Sends whatever has been `Write(...)`ten since the last `Flush()`/`FlushEnd()` as one HTTP
    chunk, then resets the body buffer so the next round of `Write(...)` calls starts clean.

    Must be `co_await`ed, and only makes sense after `FlushStart()`. It suspends the calling
    coroutine only if the socket isn't immediately writable, real backpressure from the client,
    not a fixed delay, otherwise it resumes in the same frame without ever yielding.

    Calling `Flush()` with nothing written since the last round is a harmless no-op: nothing goes
    out, and the same reserved framing stays ready for whenever real data eventually shows up.

    **Example** (streaming a database export without buffering the whole result set):
    ```cpp
    WFX_GET("/export.csv", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
        res.Header("Content-Type", "text/csv");
        res.FlushStart();
        res.Write("id,name\r\n");

        auto stream = Db.Stream(100, "SELECT id, name FROM users ORDER BY id");
        while(true) {
            auto chunk = co_await stream.Next();
            if(chunk.status != WFX::EpOk || chunk.done)
                break;

            for(std::uint32_t i = 0; i < chunk.data->RowCount(); i++) {
                auto row = chunk.data->At(i);
                res.Write(row.Get<std::int64_t>("id"))
                    .Write(",")
                    .Write(row.Get<std::string_view>("name"))
                    .Write("\r\n");
            }

            co_await res.Flush();
        }

        co_await res.FlushEnd();
        co_return;
    });
    ```

    !!! tip "Why this stays memory-bounded"
        Each round only ever holds one batch's worth of formatted text in memory. `Flush()`
        suspends the coroutine until the client has actually drained the previous round before the
        next `stream.Next()` pulls more rows, so a slow client naturally throttles how fast rows
        get fetched from the database. Peak memory is bounded by batch size, not by result-set
        size, which is what lets a multi-gigabyte export stream to a browser download in a fixed,
        small amount of RAM.

- **`FlushEnd()`** *(awaitable)*  
    Same as `Flush()`, sends whatever's pending as one last chunk, but also emits the chunked
    terminator and finalizes the response. Call this once, in place of `Commit()`, right after the
    loop that calls `Flush()` finishes.

    !!! danger
        If a handler that called `FlushStart()` returns without ever calling `FlushEnd()`, WFX
        still terminates the chunked body cleanly on your behalf, the client never hangs or sees a
        malformed response, but the response ends up silently truncated wherever the handler
        stopped. This is a bug in the handler, not something to rely on: always
        `co_await res.FlushEnd()` once the loop is done, even on an error path.