# Async

WFX async is a **manual, explicit coroutine system** built on top of a small runtime interface.  
There is no compiler magic and no hidden scheduler. What you write is exactly what runs.

Async in WFX is **state-machine–driven**, resumable, and strictly controlled by the engine.

!!! important
    All async functionality in WFX lives inside the `Async::` namespace.

    If you want to **directly use built-in async functions** (without macros), such as `SleepFor` or any other async helpers, you **must** include:

    ```cpp
    #include <async/builtins.hpp>
    ```

    For easier and more readable async code, you should also include:

    ```cpp
    #include <async/macros.hpp>
    ```

    This enables the async macros (`CoSelf`, `CoAwait`, `CoFetch`, etc.). They are purely **optional** and exist only for **convenience**; all async behavior can be implemented without them using the core `Async::` APIs.
---

## Overview

- Async is **cooperative**
- No threads are spawned implicitly
- Execution is deterministic based on your state logic

Async coroutines:

- run until they **yield**
- resume when the engine schedules them
- finish exactly once

---

## Core interface

WFX async is built around `Async::CoroutineBase`.  
You generally **do not implement `CoroutineBase` directly**, but its public functions are used frequently when writing manual async code. Even if you rely on macros, understanding this helps for advanced control.

### `Async::Error`

```cpp
enum class Error {
    NONE,
    TIMER_FAILURE,
    IO_FAILURE,
    INTERNAL_FAILURE
};
```

- **`NONE`**: No error occurred
- **`TIMER_FAILURE`**: Timer registration or scheduling failed (e.g., `Async::SleepFor` could not schedule)
- **`IO_FAILURE`**: Async I/O operation failed (network, file, etc.)
- **`INTERNAL_FAILURE`**: Runtime or programmer error inside the async system

### `Async::CoroutineBase`

```cpp
struct CoroutineBase {
public:
    CoroutineBase()          = default;
    virtual ~CoroutineBase() = default;

public:
    void          IncState()                       { /* ... */ }
    void          SetState(std::uint32_t newState) { /* ... */ }
    std::uint32_t GetState()                       { /* ... */ }

    void          SetYielded(bool yielded)         { /* ... */ }
    bool          IsYielded()                      { /* ... */ }
    void          Finish()                         { /* ... */ }
    bool          IsFinished()                     { /* ... */ }

    void          SetError(Error e)                { /* ... */ }
    Error         GetError() const                 { /* ... */ }
    bool          HasError() const                 { /* ... */ }

public: // Contract
    // Main
    virtual void            Resume()                  noexcept = 0;

    // Storage
    virtual LocalVariable&  PersistLocal(const char*) noexcept = 0;

    // Return Values
    virtual void            SetReturnPtr(void*)       noexcept = 0;
    virtual void*           GetReturnPtr()            noexcept = 0;
    virtual TypeInfo        GetReturnType()     const noexcept = 0;

private: // Internals
    ...
};
```

**Base methods**:

- **`State machine`**:  
    `GetState`, `SetState`, and `IncState` track which step the coroutine is in. This is used with switch-case to manage execution flow.

- **`Yielding`**:  
    `SetYielded` / `IsYielded` indicate if the coroutine has temporarily paused (yielded) while waiting for another async operation.

- **`Completion`**:  
    `Finish` / `IsFinished` mark when a coroutine is done. A finished coroutine will never resume.

    !!! important
        **This applies only to manual async implementations (explicit state machines / switch-case).  
        If you are using the async macro system, this requirement is handled automatically and does not apply.**

        Calling `Finish()` is **mandatory** on every execution path that completes a coroutine.  
        Even if you rely on a `default:` case or an early return, `Finish()` **must** be invoked somewhere in your state machine when the coroutine is logically done.

        If a code path is known to terminate the coroutine, you are required to call `Finish()` on that path.

        Failing to call `Finish()` will leave the coroutine marked as incomplete. The engine will continue waiting for an event to wake it up, which will never happen.  
        The coroutine becomes permanently stuck, and the associated connection will hang indefinitely.

- **`Errors`**:  
    `SetError`, `GetError`, `HasError` let you track runtime failures.  

    !!! note
        Errors are stored internally and automatically propagated via `Async::Await`.  
        Exceptions must not be thrown; if you do, they need to be handled manually within your coroutine code.

**Runtime contract**:  

- `Resume()`: Resumes execution at current state.
- `PersistLocal(name)`: Store variables that persist across yields. Details are explained later on this page.
- `SetReturnPtr(ptr)`, `GetReturnPtr()`, `GetReturnType()`: Manage optional return values.  

    !!! danger
        The pointer passed to `SetReturnPtr` **must remain valid for the entire lifetime of the coroutine**.  
        If the pointer points to a short-lived variable, the engine will crash when it tries to write the return value.  

        It is **highly recommended** to use variables that survive yields, such as those stored via `PersistLocal`, or any mechanism that guarantees the variable lives until the coroutine finishes.

`Async::CoroutineBase` is small and efficient (<=16 bytes) and forms the foundation for all WFX async operations.

### `AsyncPtr`

- Alias for `Async::CoroutineBase*`
- Represents a registered, engine-managed coroutine
- Used to:
    - Resume or await coroutines
    - Access state, check errors
    - Pass to `Async::Await` or `Async::Call`

!!! warning
    `AsyncPtr` is a non-owning pointer to a registered coroutine managed by the engine.  
    Accessing it after the coroutine has finished is invalid and can lead to undefined behavior or crashes, because the engine may have already cleaned up its memory.

---

## Persistent Storage

`Async::CoroutineBase` member function `PersistLocal` provides **per-coroutine persistent storage** that survives yields. Each coroutine can store data safely without worrying about lifetime across asynchronous suspension points.

It returns an `Async::LocalVariable` object, which acts as a type-erased container for your data.

**`Async::LocalVariable` overview**:

- Can store trivial types directly in-place or larger/non-trivial types on the heap.
- Ensures **automatic destruction** for heap-allocated objects when the coroutine finishes.
- Always returns a **reference** to the stored value via `InitOrGet<T>()`.
- Cannot store references (`T&`) directly.

**Main Function**:
```cpp
template<typename T, typename... Args>
T& InitOrGet(Args&&... args);
```

- Initializes the stored value if it hasn't been initialized yet.
- If already initialized, returns the existing reference.
- Accepts constructor arguments for initialization (`Args&&...`).
- Automatically chooses **trivial vs heap** storage based on type size and triviality.

!!! note
    - Use `PersistLocal("name").InitOrGet<T>()` to store/retrieve coroutine-local values.
    - Values persist across yields until the coroutine finishes.
    - **Trivial types** (small, trivially copyable) are stored in-place (`trivial` union member).
    - **Non-trivial or large types** are stored on the heap (`heapObj` pointer).

---

## Runtime functions

These functions are the **only supported way** to create and control async execution.

### `Async::MakeAsync`

```cpp
template<typename Ret, typename Fn, typename... Args>
inline AsyncPtr MakeAsync(Fn&& fn, Args&&... args);
```

`MakeAsync` is the core async primitive. It creates a coroutine by pairing:

- a **callable** (`Fn`): the coroutine body
- a set of **arguments** (`Args...`): data retained for the coroutine's entire lifetime

It registers the coroutine with the engine and returns an `AsyncPtr`, which is a **non-owning handle** to the currently registered coroutine instance.

**Callable contract**  

The callable must have this signature:

```cpp
void(AsyncPtr self, Args... args)
```

- `self` is a **non-owning handle** to the coroutine instance currently managed by the engine.
- `args` are the values originally provided to `MakeAsync`, automatically forwarded from the coroutine’s internal storage.

  Because the coroutine owns this storage (see **Argument lifetime rules** below), the callable may safely accept
  `args` by value or by reference (`T` / `T&` / `const T&`).

!!! warning
    Do **not** declare coroutine callable parameters as rvalue references (`T&&`).

    The callable does not own argument storage.  
    Binding to `T&&` allows the value to be moved-from on first use, which breaks correctness when the coroutine
    **yields and resumes**.

    Always use value types (`T`) or references (`T&` / `const T&`) instead.

!!! important
    All values provided to `MakeAsync` through its `Args...` parameter pack are **copied** or **moved from rvalues** (via `std::move`) when the coroutine is created.  
    The coroutine wrapper **owns this storage** for the entire lifetime of the coroutine.

    The coroutine never stores references implicitly.

    If references are required, they must be explicitly wrapped using `std::ref`, and you **must guarantee** that the referenced objects outlive the coroutine.  
    Violating this requirement results in undefined behavior.

**Return values**

- The coroutine function (**callable**) itself **always returns** `void`
- If the coroutine function needs to return a value to the caller:
    - You must specify the return type via the template parameter `Ret`
    - The engine writes the result through the pointer previously set by `SetReturnPtr`

- There is **no implicit conversion**:
    - Return type must match `Ret` exactly
    - Mismatches are logic errors and cause **immediate engine termination** (validated by `Async::Await`)

**Examples**:

The following examples **do not use the async macro system**.
They intentionally show manual async control flow to make the behavior of `Async::MakeAsync`, return values, and coroutine lifetime fully explicit.

The **same examples will be shown later using the macro system**, once the underlying mechanics are understood.

- **Coroutine with no return value (void)**  
    This example shows a coroutine performing side effects without returning any value.

    **`LogAsync` returns nothing**:
    ```cpp
    AsyncPtr LogAsync()
    {
        return Async::MakeAsync<void>(
            [](AsyncPtr self) {
                // Perform some async operation, e.g., logging
                Logger::Info("Async operation executed");

                self->Finish();  // mark coroutine as complete
            }
        );
    }
    ```

    **Caller awaiting the void coroutine**:
    ```cpp
    /* ... */
    // Caller awaiting the coroutine
    if(Async::Await(self, LogAsync(), nullptr)) {
        /* Coroutine yielded; return to engine */
        self->SetState(2);
        return;
    }

    if(self->HasError()) {
        res.Status(HttpStatus::INTERNAL_SERVER_ERROR)
            .SendText("Internal Server Error");
        self->Finish();
        return;
    }

    // No return value to retrieve; side effect completed
    /* ... */
    ```

    **Explanation**:

    For `void` coroutines, no return pointer is provided. The coroutine may perform side effects freely but **must still call** `Finish()` on all completion paths to signal to the engine that it is done.

- **Coroutine returning a value**  
    This example demonstrates how a coroutine returns a value and how the caller retrieves it.

    **`TestAsync` returning value `std::uint32_t{2000}`**:
    ```cpp
    AsyncPtr TestAsync()
    {
        return Async::MakeAsync<std::uint32_t>(
            [](AsyncPtr self) {
                // Attempt to obtain the coroutine's return pointer
                if(auto* ret = Async::SafeCastReturnPtr<std::uint32_t>(self))
                    *ret = 2000;  // write the value to the caller's variable
                else
                    self->SetError(Async::Error::INTERNAL_FAILURE);

                self->Finish();  // mark coroutine as complete
            }
        );
    }
    ```
    
    **Caller retrieving the return value into `timer` variable**:
    ```cpp
    /* ... */
    // Allocate a variable in the coroutine's persistent storage
    auto& timer = self->PersistLocal("timer").InitOrGet<std::uint32_t>(0);

    if(Async::Await(self, TestAsync(), &timer)) {
        /* Coroutine yielded; return to engine */
        self->SetState(1);
        return;
    }

    if(self->HasError()) {
        res.Status(HttpStatus::INTERNAL_SERVER_ERROR)
            .SendText("Internal Server Error");
        self->Finish();
        return;
    }

    // At this point, 'timer' contains the value returned by TestAsync()
    /* ... */
    ```

    **Explanation**:

    `PersistLocal("timer").InitOrGet<std::uint32_t>(0)` allocates `timer` inside the coroutine's internal storage, not the stack. If it already exists, it returns the existing value; otherwise, it initializes it with the provided value (`0`). This ensures the variable **remains valid across yields and resumes**.

    `Async::Await` stores the caller’s `timer` address inside the coroutine wrapper as the return pointer.  
    Inside the coroutine, `SafeCastReturnPtr<Ret>` retrieves this stored pointer as the correct type.  

    - If the cast succeeds, the coroutine writes the return value into the caller's variable.
    - If it fails, an internal error is set.
    
    Calling `Finish()` marks the coroutine complete so the engine can resume the caller with the value safely.

### `Async::Call`

```cpp
template<typename Ret, typename Fn, typename... Args>
inline AsyncPtr Call(Fn&& fn, Args&&... args);
```

`Async::Call` is a **convenience wrapper** around `Async::MakeAsync`. It allows you to define the coroutine body **directly as a free-standing function** instead of wrapping it inside a lambda every time.

**How it works**:

- The callable passed to `Async::Call` must satisfy the same callable contract as `Async::MakeAsync`.
- `Async::Call` simply forwards its arguments to `Async::MakeAsync` using `std::forward`.
- It returns an `AsyncPtr`, just like `Async::MakeAsync`.

**Examples**:

- **Coroutine with no arguments**  
    Instead of defining a wrapper function that creates the coroutine:
    ```cpp
    // Define a wrapper function that creates the coroutine
    AsyncPtr TestAsync()
    {
        return Async::MakeAsync<std::uint32_t>(
            [](AsyncPtr self) {
                if(auto* ret = Async::SafeCastReturnPtr<std::uint32_t>(self))
                    *ret = 2000;
                else
                    self->SetError(Async::Error::INTERNAL_FAILURE);

                self->Finish();
            }
        );
    }

    // Invoke the coroutine constructor
    TestAsync();
    ```

    You can instead define the coroutine as a free-standing function and create it using `Async::Call`:
    ```cpp
    // Define the coroutine body as a free-standing function
    void TestAsyncCoroutine(AsyncPtr self)
    {
        if(auto* ret = Async::SafeCastReturnPtr<std::uint32_t>(self))
            *ret = 2000;
        else
            self->SetError(Async::Error::INTERNAL_FAILURE);

        self->Finish();
    }

    // Create the coroutine
    Async::Call<std::uint32_t>(TestAsyncCoroutine);
    ```

- **Coroutine with arguments**
    ```cpp
    void WaitCoroutine(AsyncPtr self, std::uint32_t seconds)
    {
        // Coroutine body uses 'seconds'
        Logger::Info("Waiting ", seconds, " seconds...");
        self->Finish();
    }

    // Call with arguments
    Async::Call<void>(WaitCoroutine, 10);
    ```

!!! note
    - A coroutine function cannot be invoked directly like a normal C++ function.  
    `Async::Call` exists to adapt a free-standing coroutine function into a managed coroutine by internally forwarding it to `Async::MakeAsync`, allowing the engine to create the coroutine instance, assign an `AsyncPtr`, and manage execution state, lifetime, and return handling.
    - It is purely a convenience wrapper to avoid repeatedly writing lambdas for free-standing coroutine functions.
    - All argument forwarding, type safety, and coroutine storage behavior remain the same as with `Async::MakeAsync`.

### `Async::Await`

```cpp
template<typename Ret>
inline bool Await(AsyncPtr self, AsyncPtr callResult, Ret* returnIfAny) noexcept;
```

`Async::Await` is the **core scheduling and synchronization primitive** of the async system.

It is responsible for:

- resuming a child coroutine
- determining whether the current coroutine must **yield**
- wiring return values between coroutines
- propagating errors correctly
- enforcing async invariants at runtime

Every coroutine-to-coroutine interaction ultimately goes through `Async::Await`.

**Parameters**:

- `self`  
    A non-owning handle to the *current* coroutine (the caller).

- `callResult`  
    The coroutine being awaited (usually created via `Async::MakeAsync` or `Async::Call`).

- `returnIfAny`:  
    Pointer to storage where the awaited coroutine may write its return value.

    - Must be `nullptr` for `void` coroutines.
    - Must point to **persistent storage**, not a stack variable.

**Execution rules and guarantees**:

The following rules describe **observable behavior** of `Async::Await`.  
They are enforced at runtime and define what callers may and may not do.

1. **Re-entrancy is forbidden (fatal)**  
    Calling `Async::Await` while the current (caller) coroutine is already yielded is a **programmer error**.

    This indicates:

    - missing or incorrect state transitions
    - double-await without an intervening resume
    - broken coroutine control flow

    The engine **terminates immediately**.

2. **Awaiting an invalid coroutine**  
    If `callResult` is invalid:

    - an internal failure error is set on the current coroutine
    - the await is treated as **completed synchronously**
    - `Async::Await` returns `false`

    Caller logic does not change. Error handling continues through the normal propagation path.

3. **Return type must match exactly (fatal)**  
    If `returnIfAny` is non-null, strict return type validation is enforced.

    - The awaited coroutine's declared return type **must exactly match** `Ret`
    - No implicit conversion

    Any mismatch is a logic error and **terminates the engine**.

    !!! note
        If the awaited coroutine has a return value but you pass `nullptr` to `returnIfAny`, the value is simply ignored. No error occurs - you are explicitly indicating that you don't care about the returned value.

4. **Return contract**  
    - If `Async::Await` returns `true`:
        - the current coroutine has been marked as `yielded`
        - control **must** return to the engine immediately

        Failing to return after a `yielded` await is **undefined behavior**.

    - If `Async::Await` returns `false`:
        - no yield occurred
        - execution may continue normally
        - the return value (*if any*) is already written
        - errors (*if any*) are already propagated

**Example**:

This example shows a **single coroutine** handling both synchronous and asynchronous completion correctly using an explicit state machine.  
This is the **intended and required** usage pattern for `Async::Await`.

```cpp
/*
    * ExampleCoroutine() will not return anything as a coroutine
    * FastAsync() may complete synchronously + void return type
    * SlowAsync() will yield and resume later + std::uint32_t return type
    *
    * Both are awaited safely using the same control flow.
    * This example demonstrates:
    *  - correct state management
    *  - mandatory yield handling
    *  - error propagation handling
    *  - defensive default state handling
    */

void ExampleCoroutine(AsyncPtr self)
{
    // Persistent storage (must survive yields)
    auto& value = self->PersistLocal("value").InitOrGet<std::uint32_t>(0);

    // Switch on the coroutine's current state (starts at 0 by default)
    switch(self->GetState())
    {
        case 0:
        {
            // First await: guaranteed not to propagate errors
            if(Async::Await(self, FastAsync(), nullptr)) {
                self->IncState();

                /* On next `Resume()`, execution continues from `case 1:` */

                return; // REQUIRED
            }

            // Completed synchronously
            [[fallthrough]];
        }

        case 1:
        {
            // Second await: may propagate errors
            if(Async::Await(self, SlowAsync(), &value)) {
                self->IncState();
                
                /* On next `Resume()`, execution continues from `case 2:` */
                
                return; // REQUIRED
            }

            // Await completed synchronously, check for failure
            if(self->HasError()) {
                // Error already propagated by Await
                self->Finish();
                return;
            }

            [[fallthrough]];
        }

        case 2:  // All awaits completed successfully
        default: // Ensure completion even if state is unexpected
        {
            UseValueForSomeTask(value);
            self->Finish();
            return;
        }
    }
}
```

**Explanation**:

This switch-case + `GetState()` / `IncState()` pattern forms the core of WFX's coroutine machinery:

- Each `case` represents a step in the coroutine.
- `Async::Await` is called for an awaited operation.
    - If it returns `true`, the coroutine **yields**.
    - `IncState()` increments the state so that when resumed, the switch jumps to the next step automatically.
- `PersistLocal` stores variables across yields.
- When the awaited coroutine finishes synchronously (`Await` returns `false`), execution **falls through** to the next case immediately.
- This creates a single-threaded, state-driven async flow without blocking the engine, giving synchronous-looking code an asynchronous execution model.

### `Async::SafeCastReturnPtr`

```cpp
template<typename T>
inline T* SafeCastReturnPtr(AsyncPtr self) noexcept;
```

`Async::SafeCastReturnPtr` is a **runtime-safe helper** for retrieving the return value of a coroutine.

**Purpose**:

- Casts the coroutine's own return pointer to the requested type `T`.
- Ensures the cast is performed **only if the return type matches** `T` exactly.
- Prevents unsafe memory access and undefined behavior due to type mismatches.

**Parameters**:

- `self`   
    The coroutine instance retrieving its return value pointer.  
    This is **almost always the current coroutine** (`self`) because the caller sets the address of its variable inside the coroutine frame.

**Return Value**:

- Pointer of type `T*` to the coroutine's return storage if types match.
- Returns `nullptr` if:
    - `self` is invalid (`nullptr`), or
    - The return type **does not exactly match** `T`.

!!! note
    `Async::SafeCastReturnPtr` is the standard way for a coroutine to return a value to its caller.  
    Always check for `nullptr` before writing, as the pointer may be absent if no return value was requested.  
    This is effectively the **only safe method** to return values in WFX async coroutines.

!!! tip
    See the examples in [Async::MakeAsync](#asyncmakeasync) for how to implement returning a value from a coroutine using `Async::SafeCastReturnPtr`.

---

## Macros

The macros provide a **lightweight, user-friendly syntax** for writing coroutines in WFX without manually managing switch-case state machines, `PersistLocal` storage, or `Async::Await` boilerplate.

- `CoSelf`  
    ```cpp
    #define CoSelf AsyncPtr __AsyncSelf
    ```
    A local alias for the coroutine's handle inside your coroutine body.  
    Use this as the first parameter of your coroutine function to get access to the current `AsyncPtr`.

    !!! important
        All async macros are **hardcoded to operate on the identifier `__AsyncSelf`**.
        While you *can* manually declare `AsyncPtr __AsyncSelf` in the function signature,
        using any other name will silently break macro behavior.

        The callable signature itself is not violated, but **macro expansion assumes
        `__AsyncSelf` exists** and will fail logically if it does not.

        Use `CoSelf` to guarantee correctness and avoid fragile boilerplate.

- `CoStart` / `CoEnd`  
    ```cpp
    #define CoStart ...
    #define CoEnd ...
    ```
    Wraps the coroutine body in a **switch-based state machine**:
    - `CoStart`: initializes the switch on the coroutine's state (`GetState()`), starting at `0`
    - `CoEnd`: ensures that any unexpected state completes the coroutine safely by calling `Finish()`

    **Example**:
    ```cpp
    CoStart
        // Coroutine logic here
    CoEnd
    ```

- `Awaiting`  
    ```cpp
    #define CoAwait(awaitable, onError)            ...
    #define CoFetch(awaitable, returnVar, onError) ...
    ```
    `CoAwait(awaitable, onError)`: await a coroutine with no return value
    `CoFetch(awaitable, returnVar, onError)`: await a coroutine and store its return value directly in `returnVar` (simply provide the variable name; no `&` needed)

    Both macros:

    - Automatically handle yielding if the awaited coroutine does not finish synchronously.
    - Increment the state counter automatically (`SetState()` via `__COUNTER__`).
    - Propagate errors using the provided `onError` block if the awaited coroutine fails.

    **Example**:
    ```cpp
    /*
     * `timer` is stored in coroutine-local persistent storage
     */
    CoFetch(TestAsync(), timer, {
        res.Status(HttpStatus::INTERNAL_SERVER_ERROR)
            .SendText("Internal Server Error");
    })

    CoAwait(Async::SleepFor(timer), {
        res.Status(HttpStatus::INTERNAL_SERVER_ERROR)
            .SendText("Internal Server Error");
    })
    ```

- `Returning a value`  
    ```cpp
    #define CoReturn(val) ...
    ```
    - Safely writes a value to the caller using `Async::SafeCastReturnPtr`.
    - Automatically calls `Finish()` after returning.
    - Sets an internal error if the return pointer is invalid.

    **Example**:
    ```cpp
    CoReturn(std::uint32_t(2000))
    ```

- `Error handling`  
    ```cpp
    #define CoGetError()     __AsyncSelf->GetError()
    #define CoSetError(err)  __AsyncSelf->SetError(err)
    ```
    - `CoGetError()`: retrieves the current coroutine error
    - `CoSetError(err)`: sets an error on the current coroutine

- `Coroutine-local variables`  
    ```cpp
    #define CoVariable(type, name, ...) ...
    ```
    - Uses `PersistLocal` to create **persistent storage** across yields.
    - Returns a reference to the variable via `InitOrGet<T>()`.

    **Example**:
    ```cpp
    CoVariable(std::uint32_t, timer, 0) // initialized to 0
    ```

!!! important
    Most async macros (`CoAwait`, `CoFetch`, `CoReturn`, `CoVariable`) **already include the terminating semicolon** internally.

    Adding an extra `;` is **harmless but redundant**.

    The only exceptions are:

    - `CoGetError()`
    - `CoSetError(err)`

    as they are designed to be used inside expressions, conditions, or custom control flow.

    !!! warning
        `CoStart` and `CoEnd` **must be written exactly as provided**.  
        **Do not** wrap them in parentheses and **do not** append a semicolon under any circumstance.  
        Adding parentheses or a semicolon will **break the switch-case control flow** and result in incorrect or undefined coroutine behavior.

**Full Example**:
```cpp
#include <http/routes.hpp>
#include <async/builtins.hpp>
#include <async/macros.hpp>

// Coroutine returning a value synchronously
AsyncPtr TestAsync()
{
    return Async::MakeAsync<std::uint32_t>([](CoSelf) {
        CoReturn(std::uint32_t(2000))
    });
}

// HTTP route demonstrating async control flow
WFX_GET("/async-get", [](CoSelf, Request& req, Response& res) {
    // Coroutine-local persistent storage
    CoVariable(std::uint32_t, timer, 0)

    CoStart

    // Await coroutine and store its return value in `timer`
    CoFetch(TestAsync(), timer, {
        res.Status(HttpStatus::INTERNAL_SERVER_ERROR)
           .SendText("Internal Server Error - R1");
    })

    // Sleep for `timer` seconds
    CoAwait(Async::SleepFor(timer), {
        res.Status(HttpStatus::INTERNAL_SERVER_ERROR)
           .SendText("Internal Server Error - R2");
    })

    // Executed only after all awaits complete successfully
    res.SendText("Hello World!");

    CoEnd
});
```

---

## Builtins

Builtins are **predefined async helpers** provided by WFX for common operations such as sleeping, scheduling, etc.

They return an `AsyncPtr` and are designed to be awaited using `Async::Await` or the async macros (`CoAwait`, `CoFetch`).

Builtins follow the same rules as user-defined coroutines:

- They may complete **synchronously or asynchronously**
- They may **yield**
- Errors (if any) are propagated through `Async::Await`

This section documents each builtin, its behavior, and its expected usage.

### `Async::SleepFor`

```cpp
AsyncPtr SleepFor(std::uint32_t delayMs) noexcept;
```

**Description**  
Suspends the current coroutine for `delayMs` milliseconds

**Input**

- `delayMs`: Duration to sleep, in milliseconds

**Output**

- No return value (`void`)

**Error handling**

- If the timer cannot be scheduled:
    - `Async::Error::TIMER_FAILURE` is set
    - The coroutine completes immediately

- The error is propagated to the caller via `Async::Await`