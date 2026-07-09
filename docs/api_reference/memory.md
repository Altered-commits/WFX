# Memory

WFX ships its own allocator, backed by the same per-worker TLSF pool the engine
itself allocates from. Using it instead of raw `new`/`delete`/`malloc` isn't
mandatory, your code still works fine either way, but it's strongly
recommended: pool allocations are faster than going through the general-purpose
heap, and it's the same interface the engine itself uses internally, so route
handlers and the engine end up allocating through the same fast path.

!!! important
    All memory functionality lives inside the `WFX::` namespace.

    To use it, include:
    ```cpp
    #include <wfx/memory.hpp>
    ```

---

## Overview

```cpp
WFX::Alloc(size)        // raw allocation, like malloc
WFX::Realloc(ptr, size) // raw resize, like realloc
WFX::Free(ptr)          // raw free

WFX::New<T>(args...)    // allocate + construct, like `new T(args...)`
WFX::Delete(ptr)        // destroy + free, like `delete ptr`

WFX::Allocator<T>       // std::allocator-compatible, stateless
WFX::Vector<T>          // std::vector<T, WFX::Allocator<T>>
WFX::String             // std::basic_string<char, ..., WFX::Allocator<char>>
```

Use `Alloc`/`Realloc`/`Free` when you want raw bytes, `New`/`Delete` when you
want a constructed object, and `Vector`/`String` when you want a standard
container backed by the same allocator instead of the global heap.

---

## Raw allocation

```cpp
void* p = WFX::Alloc(128);
if(!p) {
    // allocation failed, handle it
}

p = WFX::Realloc(p, 256);
WFX::Free(p);
```

- `Alloc` returns `nullptr` on failure, same as `malloc`. Always check it.
- `Free` is a no-op on `nullptr`, safe to call unconditionally.
- `Realloc` follows normal `realloc` semantics: it may return a different
  pointer, and the old one must not be used again after the call.

---

## Allocating objects

`WFX::New<T>` allocates via `WFX::Alloc` and placement-constructs `T` with
whatever arguments you pass through. `WFX::Delete` destroys the object and
frees it, mirroring a normal `new`/`delete` pair:

```cpp
struct SlotState {
    int retries = 0;
};

auto* state = WFX::New<SlotState>();
// ... use state ...
WFX::Delete(state);
```

`New` forwards its arguments straight to `T`'s constructor:

```cpp
auto* p = WFX::New<MyType>(arg1, arg2);
WFX::Delete(p);
```

- `New` returns `nullptr` if the underlying allocation fails, it does not throw.
  Check the result before dereferencing it.
- `Delete` is a no-op on `nullptr`, safe to call unconditionally.
- Never mix allocators: an object obtained from `WFX::New` must be freed with
  `WFX::Delete`, never `delete` or `free`, and vice versa for objects you
  allocate with `new`.

This is exactly the pattern the engine itself uses for per-connection and
per-request state in [Endpoint](endpoint/overview.md#endpointdesc) callbacks
like `createSlotState`/`destroySlotState` and `createOutput`/`destroyOutput`.

---

## Containers

`WFX::Allocator<T>` is a stateless, standard-conforming allocator. `WFX::Vector<T>`
and `WFX::String` are the same `std::vector`/`std::basic_string` you already
know, just parameterized with it instead of the default allocator:

```cpp
WFX::Vector<int> v;
v.push_back(1);
v.push_back(2);

WFX::String s = "hi";
s += " there";
```

They behave exactly like their `std::` counterparts in every other respect,
iterators, growth behavior, exceptions on `allocate` failure (via
`std::bad_alloc`), all of it. Only where the memory comes from changes.

Because `Allocator<T>` is stateless, any two `WFX::Allocator` instances compare
equal, so containers built with it can be moved, swapped, and compared the same
way their default-allocator equivalents can.

---

## Where the memory actually comes from

`WFX::Alloc`/`New`/`Vector`/`String` all ultimately go through a per-worker
buffer pool, not the global heap. This keeps user-space allocations on the
same fast, pre-sized pool the engine itself relies on for request handling.

!!! danger
    The pool is initialized by the worker process at startup, **after** the
    user library's static initializers have already run. If a namespace-scope
    object built before `Run()` allocates through `WFX::Alloc` (directly, or
    indirectly through `WFX::New`, `WFX::Vector`, or `WFX::String`) during
    static initialization, it can end up allocating from a pool that doesn't
    exist yet.

    In practice this only bites values large enough to actually need a heap
    allocation. A `WFX::String` short enough to fit in its small-string buffer
    never touches the pool and is completely safe to build at namespace scope,
    a long one silently allocates against an uninitialized pool. If you need a
    long-lived string or buffer at namespace scope (a hostname, a config value,
    and similar), prefer a plain `std::string_view` over a static buffer, or a
    `const char*` literal, and only build `WFX::String`/`WFX::Vector` values
    once you're inside a route handler or a callback the engine invokes after
    startup.
