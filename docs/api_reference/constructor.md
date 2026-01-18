# Constructor

WFX constructors provide a simple way to run **one-time user code at engine startup**.

A constructor is the **first user-level callback executed**, before any other user macros such as routes, middleware, or similar registrations.

!!! important
    Constructor requires the user to always include the following header at the top of the file:
    ```cpp
    #include <core/constructor.hpp>
    ```

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