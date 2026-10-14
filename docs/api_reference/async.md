# Async

WFX async is a **manual, explicit coroutine system** built directly on top of
standard C++20 coroutines using `Task<>` and `Promise<>` types.

Everything is driven by explicit `co_await` suspension and resumption.

!!! important
    All async functionality in WFX lives inside the `WFX::` namespace.

    Built-in async functions such as `SleepFor` come from `<wfx/async.hpp>`, which
    `<wfx/http.hpp>` already includes, so a handler file needs nothing extra:

    ```cpp
    #include <wfx/http.hpp>
    ```
---

## Overview

- Async is cooperative
- No threads are spawned implicitly
- Execution is deterministic and engine-driven

Async coroutines:

- run until they `co_await`
- suspend explicitly
- resume only when the engine schedules them
- finish exactly once

A coroutine of your own that another one `co_await`s returns `WFX::Task<T>`, where `T` is what it
`co_return`s (`WFX::Task<>` for nothing). `WFX::Coro` and `WFX::MwCoro` are the same thing for route
handlers (`Task<void>`) and middleware (`Task<WFX::MiddlewareAction>`):

```cpp
WFX::Task<int> CountUsers() { /* ... */ co_return 42; }

WFX_GET("/users/count", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    const int n = co_await CountUsers();
    res.SendText(std::to_string(n));
    co_return;
})
```

An exception escaping a `WFX::Task` reaches the coroutine that `co_await`s it, at that `co_await`,
exactly like a normal call's, so a `try`/`catch` around it works. One that escapes a route handler or
middleware fails just that request with a `500`.

---

## Async Status Constants

Builtin async operations internally return `WFX::AsyncStatus` values.

However, user code should generally prefer the exported `WFX::` constexpr aliases instead of referencing the internal enum types directly.

Preferred user-facing constants:

```cpp
WFX::AsyncOk
WFX::AsyncTimerFailure
WFX::AsyncIoFailure
WFX::AsyncInternalFailure
WFX::AsyncNone
```

These map directly to the underlying engine async status values while providing a cleaner and more stable public API surface.

---

## Builtins

Builtins are **predefined awaitables** provided by WFX for common async tasks such
as sleeping, scheduling, and timing.

Builtins behave exactly like user-defined coroutines:

- They may suspend
- They may resume immediately or later
- They return a status through `co_await`

All builtins are implemented as **C++20 awaitable types** with:

- `await_ready`
- `await_suspend`
- `await_resume`

This section documents each builtin and how to use it correctly.

### `WFX::SleepFor`

```cpp
SleepForAwaitable SleepFor(std::uint32_t delayMs);
```

**Description**  
Suspends the current coroutine for `delayMs` milliseconds

**Input**

- `delayMs`: Duration to sleep, in milliseconds

**Output**

- Returns a `WFX::AsyncStatus` via `co_await`: `WFX::AsyncOk` once the delay has passed

**Error handling**

- If the timer cannot be scheduled:
    - `WFX::AsyncTimerFailure` is returned
    - The coroutine does not suspend: `co_await` returns right away

- The caller must check the returned status

**Example**

```cpp
auto status = co_await WFX::SleepFor(500);

if(status != WFX::AsyncOk) {
    // handle async failure
}
```