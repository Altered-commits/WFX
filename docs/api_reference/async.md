# Async

WFX async is a **manual, explicit coroutine system** built directly on top of
standard C++20 coroutines using `Task<>` and `Promise<>` types.

Everything is driven by explicit `co_await` suspension and resumption.

!!! important
    All async functionality in WFX lives inside the `WFX::` namespace.

    If you want to **directly use built-in async functions** such as `SleepFor`, you **must** include:

    ```cpp
    #include <wfx/async.hpp>
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

---

## Async Status Constants

Builtin async operations internally return `Shared::AsyncStatus` values.

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

- Returns an `Async::Status` via `co_await`

**Error handling**

- If the timer cannot be scheduled:
    - `Async::Status::TIMER_FAILURE` is returned
    - The coroutine completes immediately

- The caller must check the returned status

**Example**

```cpp
auto status = co_await WFX::SleepFor(500);

if(status != WFX::AsyncOk) {
    // handle async failure
}
```