# Constructor

WFX constructors provide a simple way to run **one-time user code at engine startup**.

A constructor is the **first user-level callback executed**, before any other user macros such as routes, middleware, or similar registrations.

!!! important
    If no other `wfx/*` header has already been included, the following header must be included before using `Constructor`:

    ```cpp
    #include <wfx/app.hpp>
    ```

    Most higher-level WFX headers already include this internally, so manual inclusion is typically only required when working directly with application-level functionality.
---

## What it does

- Runs **at the very start of engine initialization**
- Intended for small runtime setup tasks
- Purely a convenience mechanism

Do **not** expect it to run again.

---

## Usage

```cpp
/*
 * NOTE: The callback signature must be `void(void)`
 */

WFX_CONSTRUCTOR([] {
    // One-time startup logic
});

// Or

void InitSomething()
{
    // One-time startup logic
}

WFX_CONSTRUCTOR(InitSomething);
```

!!! note
    - No execution order guarantees
    - Heavy work inside constructor is allowed, but it will directly increase engine startup time
    - **Do not throw exceptions**; they are not handled  
    If you need to throw, catch and handle them inside the constructor body