# Metrics and Observability

WFX tracks server metrics across all worker processes using a shared memory region allocated before the fork. No locks, no atomics, no synchronization between workers. Each worker writes exclusively to its own slot.

---

## Memory layout

Before the fork, the master calls `MetricTracer::Create(workerCount)`. This allocates a contiguous shared memory region (`mmap(MAP_SHARED | MAP_ANONYMOUS)` on POSIX, `CreateFileMapping` on Windows) sized exactly to hold `workerCount` slots:

```
[ WorkerMetrics slot 0 ][ WorkerMetrics slot 1 ][ WorkerMetrics slot 2 ] ...
```

Each `WorkerMetrics` slot is `alignas(64)` and its size is always a multiple of 64 bytes. This ensures each worker's slot occupies its own set of cache lines with no overlap into adjacent slots, preventing false sharing between workers.

After the fork, each worker calls `MetricTracer::InitWorker(index)` which stores its slot index in a process-local static. From that point on, `MetricTracer::Current()` returns a direct pointer to that worker's slot.

---

## Struct layout

`WorkerMetrics` embeds all metric structs directly. The current categories are:

```
WorkerMetrics
    LogMetrics     log        (48 bytes, 6 x uint64)
    NetworkMetrics network    (120 bytes, 15 x uint64)
    SelfMetrics    self       (48 bytes)
```

All nested metric structs are defined in `shared/abis/types.hpp` because they cross the ABI boundary. User code queries them directly through `UTILS_API_EXT1`. `WorkerMetrics` itself does not cross the boundary. It lives purely on the engine side inside the mapped region.

`SelfMetrics` is written exclusively by the master process, not workers. It holds per-slot process state: resident memory, virtual memory, restart and crash counts, backoff state, PID, and timestamps. Workers read it at scrape time but never write to it.

The fields in `LogMetrics` are ordered to match `Logger::Level` exactly (trace=0, debug=1, info=2, warn=3, error=4, fatal=5). This allows the logger to increment the correct counter using pointer arithmetic rather than a switch:

```cpp
std::uint64_t* lines = &metrics_->log.trace;
lines[static_cast<std::uint8_t>(lvl)]++;
```

Adding a field to any of the nested metrics automatically grows `WorkerMetrics` to the next multiple of 64. The `static_assert` on `WorkerMetrics` enforces this at compile time.

!!! important
    The order of fields in `LogMetrics` must always match the order of `Logger::Level` values. If you add a new log level, add its counter at the matching position in `LogMetrics` and update the assert.

---

## Writing metrics

Workers write directly to their slot with no overhead:

```cpp
if(auto* m = MetricTracer::Current())
    m->network.accepts++;
```

`MetricTracer::Current()` returns a cached pointer set at `InitWorker` time. The check is a single null test against a pointer that is always in L1 cache on the hot path.

In `EpollConnectionHandler`, the pointer is cached at construction time into `metrics_` to avoid even the function call:

```cpp
Shared::WorkerMetrics* metrics_ = MetricTracer::Current();
```

Then all writes are just:

```cpp
metrics_->network.accepts++;
metrics_->network.bytesRead += n;
```

---

## Reading metrics

Any worker can read the entire mapped region at any time. `MetricTracer` provides an `Aggregate...()` function for each metric category that loops over all slots and sums the relevant fields. Each category also has a per-worker variant that returns only the current worker's slot.

The aggregate and per-worker functions for each category are exposed to user code through `UTILS_API_EXT1`, and wrapped as inline functions in `include/wfx/telemetry.hpp`. See that file for the current list of available functions.

Not every field makes sense to aggregate. For `SelfMetrics`, only `rssBytes`, `vmBytes`, `restarts`, and `crashes` are summed across slots. Fields like `pid`, `startedAt`, `nextRetryAt`, and `backoffAttempts` are per-slot only and are not included in the aggregate result.

Aggregation is not atomic. A worker reading slot N while another worker is mid-increment on slot N may see a partially updated value. For a metrics scrape endpoint this is acceptable. For anything requiring strict consistency it is not.

---

## Lifecycle

```
Master: MetricTracer::Create(N)     // shared memory region
        fork()
Worker: MetricTracer::InitWorker(i) // slot index stored process-locally
        ... runs event loop ...
Master: MetricTracer::Destroy()     // region released, hygiene only
```

If the process crashes before `Destroy()` is called, the OS reclaims the shared memory region automatically. On POSIX this is because the mapping is anonymous and not backed by a file. On Windows, all handles are closed by the OS on process termination which releases the mapping. No manual cleanup is needed.

---

## Adding a new metric

**Adding to an existing category:**

1. Append the new `uint64_t` field at the end of the struct in `shared/abis/types.hpp`.
2. Add the aggregation line in the corresponding `Aggregate...()` function in `utils/diagnostics/metric_tracer.cpp`.
3. Increment it from the right place in the engine.
4. The `static_assert` on `WorkerMetrics` will fail if the new size is not a multiple of 64. If it fails, add padding fields until it passes.

!!! important
    Never remove or reorder fields in any metric struct that crosses the ABI boundary. Reordering breaks user binaries compiled against an older version.

**Adding a new category entirely:**

1. Define a new struct in `shared/abis/types.hpp` following the same pattern as the existing ones.
2. Add it as a new embedded member in `WorkerMetrics`.
3. Add a new `Aggregate...()` function in `utils/diagnostics/metric_tracer.hpp` and implement it in `metric_tracer.cpp`.
4. Add new function pointer types and entries to `UTILS_API_EXT1` in `shared/apis/utils_api.hpp`.
5. Wire the lambdas in `shared/apis/utils_api.cpp`.
6. Add the user-facing inline functions in `include/wfx/telemetry.hpp`.

---

## Where things live

- `shared/abis/types.hpp`  
    Definitions for all metric structs and `WorkerMetrics`.

- `utils/diagnostics/metric_tracer.hpp`  
    `MetricTracer` namespace. Hot path functions (`Current`, `Slot`) are inline here. Lifecycle and aggregation functions are declared here and defined in `metric_tracer.cpp`.

- `utils/diagnostics/metric_tracer.cpp`  
    Implementations of `Create`, `InitWorker`, `Destroy`, and all `Aggregate...()` functions.

- `shared/apis/utils_api.hpp`  
    `UTILS_API_EXT1` function pointer declarations for metrics and logging.

- `shared/apis/utils_api.cpp`  
    Lambda implementations that wire `MetricTracer` to the API function pointers.

- `include/wfx/telemetry.hpp`  
    User-facing inline functions for querying metrics. This is the source of truth for what is currently available to user code.