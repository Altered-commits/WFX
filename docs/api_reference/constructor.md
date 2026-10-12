# Constructor

WFX constructors provide a simple way to run **user code once as each worker starts**.

A constructor runs in the same deferred queue as routes, middleware and endpoints, in
static-initialization order: declaration order within one file, unspecified across files. Don't
rely on it running before a registration made in another file.

!!! important
    `WFX_CONSTRUCTOR` is only available through `<wfx/app.hpp>` (or `<wfx/all.hpp>`, which
    includes it); no other `wfx/*` header pulls it in:

    ```cpp
    #include <wfx/app.hpp>
    ```
---

## What it does

- Runs once per worker process, when the worker loads your library
- Intended for small runtime setup tasks
- Purely a convenience mechanism

It doesn't run again within a worker, but every worker runs it, including one revived after a
crash.

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