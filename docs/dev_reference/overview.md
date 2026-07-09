# Overview

WFX is a C++ web framework focused on performance and simplicity. It handles connections, buffers, routing, SSL, templates, metrics, and logging so you can focus on writing handlers.

It is still in active development. Things will change.

!!! important
    WFX currently only supports Linux, and this section of the docs describes
    that implementation, including OS-specific mechanisms where the current
    backend relies on them. A future port to another OS is free to implement
    the same architectural shape (a coordinating process, independent workers,
    no shared hot-path state) with entirely different primitives underneath.

---

## Core idea

WFX separates engine state from user code. The engine owns connections, buffers, routing tables, SSL context, and timers. User code owns only the logic inside handler functions and is compiled into a shared library loaded at runtime.

This separation is what makes hot reload possible. When user code changes, the engine reloads the library without dropping connections or restarting workers.

The engine itself runs as a coordinating process plus a fixed number of independent workers, each with its own event loop and no shared state on the hot path. See [Architecture](architecture.md) for exactly how that's implemented on the current (Linux) backend.

---

## ABI boundary

User code communicates with the engine through ABI-stable function pointer structs defined in `shared/abis/`. Every struct that crosses this boundary is standard layout, contains no STL types or virtual functions, and is append-only. Fields are never removed or reordered.

When user code calls `WFX::LogInfo(...)` or registers a route, it calls through a function pointer. The implementation lives in the engine. User code never links directly against engine internals.

---

## Where to go next

- **Architecture** -> startup flow, request lifecycle, and subsystem relationships
- **Codebase Structure** -> folder layout and where to find things
- **Globals and Singletons** -> the `Get...()` functions, their lifetimes and ownership
- **ABI Layer** -> how to read and extend the API structs
- **Metrics and Observability** -> how `MetricTracer` works