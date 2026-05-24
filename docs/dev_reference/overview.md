# Overview

WFX is a C++ web framework focused on performance and simplicity. It handles connections, buffers, routing, SSL, templates, metrics, and logging so you can focus on writing handlers.

It is still in active development. Things will change.

---

## Core idea

WFX separates engine state from user code. The engine owns connections, buffers, routing tables, SSL context, and timers. User code owns only the logic inside handler functions and is compiled into a shared library loaded at runtime.

This separation is what makes hot reload possible. When user code changes, the engine reloads the library without dropping connections or restarting workers.

---

## Process model

WFX uses a master/worker fork model.

```
Master Process
    fork() x N
        Worker 0  (event loop)
        Worker 1  (event loop)
        Worker 2  (event loop)
        Worker 3  (event loop)
```

The master initializes everything then forks N worker processes. Each worker runs its own independent event loop. Workers do not share memory except for the metrics region, which is a shared `mmap` allocated before the fork.

Each worker has its own connection pool, buffer pool, file cache, and logger. There is no synchronization between workers on the hot path.

---

## ABI boundary

User code communicates with the engine through ABI-stable function pointer structs defined in `shared/abis/`. Every struct that crosses this boundary is standard layout, contains no STL types or virtual functions, and is append-only. Fields are never removed or reordered.

When user code calls `WFX::LogInfo(...)` or registers a route, it calls through a function pointer. The implementation lives in the engine. User code never links directly against engine internals.

---

## Current state

What exists today:

- HTTP/1.1 server with keep-alive
- Routing, middleware, async handlers via coroutines
- HTTPS via OpenSSL
- Form handling and parsing
- Template engine with static and dynamic template compilation
- JSON (immediate mode and retained mode)
- Per-worker TLSF buffer pool
- Structured logging with file rotation
- Shared metrics via mmap, queryable from user code
- Crash tracer with stack trace dumps
- CLI for project scaffolding

What does not exist yet:

- HTTP/2
- MacOS support (in progress)
- Windows support
- Database drivers or ORM
- Inter-worker communication

---

## Where to go next

- **Architecture** -> detailed breakdown of each subsystem
- **Codebase Structure** -> folder layout and where to find things
- **Globals and Singletons** -> the `Get...()` functions, their lifetimes and ownership
- **ABI Layer** -> how to read and extend the API structs
- **Metrics and Observability** -> how `MetricTracer` works