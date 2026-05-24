# ABI Layer

WFX compiles user code into a shared library loaded at runtime. The engine and the user library are two separate binaries that may have been compiled at different times. The ABI layer is the contract between them.

---

## The problem it solves

The engine cannot call into user code directly, and user code cannot include engine headers. If either side changed its internal types and the other side recompiled, they would break each other. The ABI layer prevents this by defining a fixed set of types and function pointer structs that both sides agree on and that never change layout.

---

## How it works

The engine exposes all its functionality through a set of function pointer structs. Each struct is a flat table of function pointers with no virtual functions, no STL types, and no templates. Every struct carries a `static_assert` confirming it is standard layout.

These structs are grouped into four APIs:

- **`HTTP_API_EXT1`** -> routing, middleware registration, request reading, response writing, endpoint management
- **`ASYNC_API_EXT1`** -> async timer registration for coroutine-based handlers
- **`MEMORY_API_EXT1`** -> `Alloc`, `Realloc`, `Free` backed by the engine's per-worker TLSF pool
- **`UTILS_API_EXT1`** -> logging and metrics

All four are bundled into a single **`MASTER_API_TABLE`**, which is one struct containing four function pointers, each returning a pointer to one of the four API structs above.

---

## Injection flow

When a worker loads the user library (`dlopen` on POSIX, `LoadLibrary` on Windows), it immediately calls `RegisterMasterAPI` which is the one exported symbol the user library must provide. The engine passes a pointer to its `MASTER_API_TABLE` into that function.

On the user side, `RegisterMasterAPI` stores the table pointer and calls any deferred initialization that was waiting for the API to be available. After this call returns, user code can call any engine function through the table.

```
Engine loads user .so
    calls RegisterMasterAPI(table)
        user stores table pointer
        user runs deferred init (route registration, middleware registration, etc.)
    RegisterMasterAPI returns
Engine continues
```

User code never calls engine functions directly. It always goes through the table:

```cpp
WFX::Core::HttpApiExt1()->RegisterRoute(...)
WFX::Core::UtilsApiExt1()->LogInfo(...)
WFX::Core::MemoryApiExt1()->Alloc(size)
```

The helper functions in `core/core.hpp` wrap these calls so user-facing APIs like `WFX::LogInfo` and `WFX_GET` never expose the table directly.

---

## ABI stability rules

These rules apply to every struct in `shared/abis/` and every API struct:

- Fields are never removed.
- Fields are never reordered.
- New fields are only ever appended at the end.
- Every struct must be standard layout. The `static_assert` enforces this at compile time.
- No STL types, no virtual functions, no templates cross the boundary.
- Strings cross as `StringView` (a plain `data` pointer and a `size`), not `std::string` or `std::string_view`.
- Pointers to engine-internal types cross as `void*` and are cast on the engine side.

The `EXT` suffix stands for extension, not version. `HTTP_API_EXT1` is the first extension of the HTTP API. If new functionality is needed that cannot fit into an existing struct without breaking layout, a new `HTTP_API_EXT2` is added alongside it. `EXT1` is not replaced or copied into `EXT2`. Both exist independently and both remain in use. Individual functions or fields that become obsolete are marked with `[[deprecated]]` but never removed.

This is how backward compatibility is maintained. A user binary compiled against `EXT1` keeps working against an engine that also has `EXT2`. Nothing is ever removed.

---

## Where things live

```
shared/abis/          ABI-stable types used by both engine and user code
shared/apis/          The four API structs and their getter functions
include/core/core.hpp User-side table storage and accessor helpers
```

The implementations of the API function pointers live on the engine side in `shared/apis/*.cpp`. User code never sees those files.