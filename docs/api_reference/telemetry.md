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

| Function | Level | Typical use |
|---|---|---|
| `LogTrace` | 0 | Fine-grained execution tracing |
| `LogDebug` | 1 | Development diagnostics |
| `LogInfo`  | 2 | Normal operational events |
| `LogWarn`  | 3 | Unexpected but recoverable situations |
| `LogError` | 4 | Failures that affect a single request |
| `LogFatal` | 5 | Unrecoverable errors, aborts immediately |

The minimum level shown is controlled by `min_level` in `wfx.toml`. Logs below that level are discarded.

### Formatting

Arguments are formatted into a 1024-byte stack buffer. No heap allocation occurs. If a line exceeds 1024 bytes it is truncated and `...` is appended.

```cpp
WFX::LogInfo("request at path=", req.Path(), " method=", req.Method());
```

**Supported argument types**: `std::string_view`, `const char*`, `char`, `bool`, integers (`int`, `uint32_t`, `uint64_t`, etc.), floats, pointers.

!!! important
    `LogFatal` does not return. It emits the log line and calls `std::abort`. Use it only for situations where the process cannot continue.

---

## Metrics

WFX tracks server metrics across all worker processes automatically. You can query them at any time from any route handler.

Two categories are available:

- **Log metrics**: how many lines were emitted at each level
- **Network metrics**: connections, bytes, requests, and response counts

### Per-worker vs all workers

Each worker process tracks its own counters independently. You can query the current worker's counters or aggregate across all workers.

| Function                      | Returns                        |
|-------------------------------|--------------------------------|
| `WFX::GetLogMetrics()`        | This worker's log counters     |
| `WFX::GetNetworkMetrics()`    | This worker's network counters |
| `WFX::GetLogMetricsAll()`     | All workers summed             |
| `WFX::GetNetworkMetricsAll()` | All workers summed             |

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
    uint64_t activeConns;       // connections currently open
    uint64_t requests;          // total HTTP requests parsed
    uint64_t response1xx;       // responses with 1xx status
    uint64_t response2xx;       // responses with 2xx status
    uint64_t response3xx;       // responses with 3xx status
    uint64_t response4xx;       // responses with 4xx status
    uint64_t response5xx;       // responses with 5xx status
};
```

!!! note
    `fileFallbacks` is incremented when a file send cannot use `sendfile` directly because the connection is SSL and kernel TLS is not available. In that case WFX falls back to streaming the file through the SSL write path in chunks. `fileCalls` and `fileBytesWritten` count both direct file sends and this fallback path.

### Usage

```cpp
WFX_GET("/metrics", [](WFX::Request req, WFX::Response res) {
    auto log = WFX::GetLogMetricsAll();
    auto net = WFX::GetNetworkMetricsAll();

    res.Header("Content-Type", "text/plain")
       .Write("log.trace=").Write(log.trace)
       .Write("\nlog.debug=").Write(log.debug)
       .Write("\nlog.info=").Write(log.info)
       .Write("\nlog.warn=").Write(log.warn)
       .Write("\nlog.error=").Write(log.error)
       .Write("\nlog.fatal=").Write(log.fatal)
       .Write("\nnet.accepts=").Write(net.accepts)
       .Write("\nnet.active_conns=").Write(net.activeConns)
       .Write("\nnet.bytes_read=").Write(net.bytesRead)
       .Write("\nnet.bytes_written=").Write(net.bytesWritten)
       .Write("\nnet.file_bytes_written=").Write(net.fileBytesWritten)
       .Write("\nnet.requests=").Write(net.requests)
       .Write("\nnet.1xx=").Write(net.response1xx)
       .Write("\nnet.2xx=").Write(net.response2xx)
       .Write("\nnet.3xx=").Write(net.response3xx)
       .Write("\nnet.4xx=").Write(net.response4xx)
       .Write("\nnet.5xx=").Write(net.response5xx)
       .Commit();
});
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
       .Write("wfx_active_connections ").Write(net.activeConns).Write("\n")
       .Write("wfx_bytes_received_total ").Write(net.bytesRead).Write("\n")
       .Write("wfx_bytes_sent_total ").Write(net.bytesWritten + net.fileBytesWritten).Write("\n")
       .Write("wfx_log_errors_total ").Write(log.error).Write("\n")
       .Write("wfx_log_fatals_total ").Write(log.fatal).Write("\n")
       .Commit();
});
```

!!! note
    - Metrics are monotonically increasing counters except `activeConns`, which reflects the current live connection count. Counters reset when the server restarts.
    - The per-worker functions (`GetLogMetrics`, `GetNetworkMetrics`) are useful for per-process dashboards or debugging worker imbalance. For production monitoring, prefer the `All` variants.
    - Total bytes sent is the sum of `bytesWritten` and `fileBytesWritten` since file sends use a separate syscall path.