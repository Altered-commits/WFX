# Telemetry

WFX provides two telemetry tools: **structured logging and server metrics**.

!!! important
    Telemetry requires the user to always include the following header at the top of the file:
    ```cpp
    #include <wfx/telemetry.hpp>
    ```

---

## Logging

Six log levels are available. Each function accepts any number of arguments and formats them into a single line. Strings, integers, floats, booleans, pointers, and `const char*` are all supported natively. No format strings, no `std::to_string`.

```cpp
WFX::LogTrace("entering handler, path=", req.Path());
WFX::LogDebug("parsed ", count, " items");
WFX::LogInfo("[Auth]: user ", userId, " logged in");
WFX::LogWarn("[Cache]: eviction pressure, size=", size);
WFX::LogError("[DB]: query failed, code=", code);
WFX::LogFatal("[Init]: cannot continue");  // aborts the process
```

### Log levels

| Function   | Level | Typical use                              |
|------------|-------|------------------------------------------|
| `LogTrace` |   0   | Fine-grained execution tracing           |
| `LogDebug` |   1   | Development diagnostics                  |
| `LogInfo`  |   2   | Normal operational events                |
| `LogWarn`  |   3   | Unexpected but recoverable situations    |
| `LogError` |   4   | Failures that affect a single request    |
| `LogFatal` |   5   | Unrecoverable errors, aborts immediately |

The minimum level shown is controlled by `min_level` in `wfx.toml`. Logs below that level are discarded.

### Formatting

Arguments are formatted into a 1024-byte stack buffer. No heap allocation occurs. If a line exceeds 1024 bytes it is truncated and `...` is appended.

```cpp
WFX::LogInfo("request at path=", req.Path(), " method=", req.Method());
```

**Supported argument types**: `std::string_view`, `const char*`, `char`, `bool`, integers (`int`, `uint32_t`, `uint64_t`, etc.), floats, pointers.

!!! danger
    `LogFatal` does not return. It emits the log line and calls `std::exit`. Use it only for situations where the process cannot continue.

---

## Metrics

WFX tracks server metrics across all worker processes automatically. You can query them at any time from any route handler.

Three categories are available:

- **Log metrics**: how many lines were emitted at each level
- **Network metrics**: connections, bytes, requests, and response counts
- **Process metrics**: Written by the master process and updated every `master_poll_interval` seconds. Workers read it at scrape time. Internally named as `SelfMetrics`.

### Per-worker vs all workers

Each worker process tracks its own counters independently. You can query the current worker's counters or aggregate across all workers.

| Function                        | Returns                        |
|---------------------------------|--------------------------------|
| `WFX::GetLogMetrics()`          | This worker's log counters     |
| `WFX::GetNetworkMetrics()`      | This worker's network counters |
| `WFX::GetProcessMetrics()`      | This worker's process info     |
| `WFX::GetLogMetricsAll()`       | All workers summed             |
| `WFX::GetNetworkMetricsAll()`   | All workers summed             |
| `WFX::GetProcessMetricsAll()`   | All workers summed             |

For a `/metrics` endpoint that reflects the whole server, use the `All` variants.

### `LogMetrics` fields

```cpp
struct LogMetrics {
    uint64_t trace;
    uint64_t debug;
    uint64_t info;
    uint64_t warn;
    uint64_t error;
    uint64_t fatal;
};
```

### `NetworkMetrics` fields

```cpp
struct NetworkMetrics {
    uint64_t accepts;           // total accepted client connections
    uint64_t reads;             // total read syscalls that transferred data
    uint64_t bytesRead;         // total bytes received
    uint64_t writes;            // total write syscalls that transferred data
    uint64_t bytesWritten;      // total bytes sent via write
    uint64_t fileCalls;         // total successful file send syscalls
    uint64_t fileFallbacks;     // file sends that fell back to streaming (SSL only, Linux)
    uint64_t fileBytesWritten;  // total bytes sent via file sends
    uint64_t activeClientConns;   // inbound client connections currently open
    uint64_t activeEndpointConns; // outbound endpoint connections currently open
    uint64_t requests;          // total HTTP requests parsed
    uint64_t response1xx;       // responses with 1xx status
    uint64_t response2xx;       // responses with 2xx status
    uint64_t response3xx;       // responses with 3xx status
    uint64_t response4xx;       // responses with 4xx status
    uint64_t response5xx;       // responses with 5xx status
};
```

### `SelfMetrics` fields

```cpp
struct SelfMetrics {
    uint64_t rssBytes;        // resident set size in bytes (physical memory in use)
    uint64_t vmBytes;         // virtual memory size in bytes
    uint32_t restarts;        // how many times this slot has been restarted
    uint32_t crashes;         // how many times this slot died unexpectedly
    uint32_t backoffAttempts; // current backoff attempt count
    int32_t  pid;             // current worker PID (-1 if dead)
    int64_t  startedAt;       // unix timestamp of last worker start
    int64_t  nextRetryAt;     // unix timestamp of next allowed restart (0 if not in backoff)
};
```

!!! important
    - `GetProcessMetricsAll()` only meaningfully aggregates `rssBytes`, `vmBytes`, `restarts`, and `crashes`. Fields like `pid`, `startedAt`, `nextRetryAt`, and `backoffAttempts` are per-slot only and are zeroed out in the aggregate result.
    - `fileFallbacks` is incremented when a file send cannot use `sendfile` directly because the connection is SSL and kernel TLS is not available. In that case WFX falls back to streaming the file through the SSL write path in chunks. `fileCalls` and `fileBytesWritten` count both direct file sends and this fallback path.

### Usage

```cpp
WFX_GET("/metrics", [](WFX::Request req, WFX::Response res) {
    auto log  = WFX::GetLogMetricsAll();
    auto net  = WFX::GetNetworkMetricsAll();
    auto self = WFX::GetProcessMetricsAll();

    auto j = WFX::ImJson(res);

    j.Obj("log");
        j.Write("trace", log.trace);
        j.Write("debug", log.debug);
        j.Write("info",  log.info);
        j.Write("warn",  log.warn);
        j.Write("error", log.error);
        j.Write("fatal", log.fatal);
    j.End();

    j.Obj("network");
        j.Write("accepts",               net.accepts);
        j.Write("bytes_read",            net.bytesRead);
        j.Write("bytes_written",         net.bytesWritten);
        j.Write("file_bytes_written",    net.fileBytesWritten);
        j.Write("active_client_conns",   net.activeClientConns);
        j.Write("active_endpoint_conns", net.activeEndpointConns);
        j.Write("requests",              net.requests);
        j.Write("response_1xx",          net.response1xx);
        j.Write("response_2xx",          net.response2xx);
        j.Write("response_3xx",          net.response3xx);
        j.Write("response_4xx",          net.response4xx);
        j.Write("response_5xx",          net.response5xx);
    j.End();

    j.Obj("process");
        j.Write("rss_bytes", self.rssBytes);
        j.Write("vm_bytes",  self.vmBytes);
        j.Write("restarts",  self.restarts);
        j.Write("crashes",   self.crashes);
    j.End();
})
```

You are not limited to plain text. Format the structs however you want: Prometheus exposition format, JSON, anything.

### Prometheus format example

```cpp
WFX_GET("/metrics", [](WFX::Request req, WFX::Response res) {
    auto net = WFX::GetNetworkMetricsAll();
    auto log = WFX::GetLogMetricsAll();

    res.Header("Content-Type", "text/plain; version=0.0.4")
       .Write("# HELP wfx_requests_total Total HTTP requests\n")
       .Write("# TYPE wfx_requests_total counter\n")
       .Write("wfx_requests_total ").Write(net.requests).Write("\n")
       .Write("wfx_responses_2xx_total ").Write(net.response2xx).Write("\n")
       .Write("wfx_responses_4xx_total ").Write(net.response4xx).Write("\n")
       .Write("wfx_responses_5xx_total ").Write(net.response5xx).Write("\n")
       .Write("wfx_active_client_connections ").Write(net.activeClientConns).Write("\n")
       .Write("wfx_active_endpoint_connections ").Write(net.activeEndpointConns).Write("\n")
       .Write("wfx_bytes_received_total ").Write(net.bytesRead).Write("\n")
       .Write("wfx_bytes_sent_total ").Write(net.bytesWritten + net.fileBytesWritten).Write("\n")
       .Write("wfx_log_errors_total ").Write(log.error).Write("\n")
       .Write("wfx_log_fatals_total ").Write(log.fatal).Write("\n")
       .Commit();
});
```

!!! note
    - Metrics are monotonically increasing counters except `activeClientConns` and `activeEndpointConns`, which reflect the current live connection counts, and `SelfMetrics` fields which are written by the master process and reflect the live state of each worker slot at the last poll interval. Counters reset when the server restarts.
    - The per-worker functions are useful for per-process dashboards or debugging worker imbalance. For production monitoring, prefer the `All` variants.
    - Total bytes sent is the sum of `bytesWritten` and `fileBytesWritten` since file sends use a separate syscall path.