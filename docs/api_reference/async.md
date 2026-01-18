# Async

WFX async is a **manual, explicit coroutine system** built directly on top of
standard C++20 coroutines using `Task<>` and `Promise<>` types.

Everything is driven by explicit `co_await` suspension and resumption.

!!! important
    All async functionality in WFX lives inside the `Async::` namespace.

    If you want to **directly use built-in async functions** such as `SleepFor`, you **must** include:

    ```cpp
    #include <async/builtins.hpp>
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

### `Async::SleepFor`

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
Async::Status status = co_await Async::SleepFor(500);

if(status != Async::Status::NONE) {
    // handle timer failure
}
```