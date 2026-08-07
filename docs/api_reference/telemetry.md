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

Several categories are available:

- **Log metrics**: how many lines were emitted at each level
- **Network metrics**: connections, bytes, and syscall counts
- **Process metrics**: Written by the master process and updated every `master_poll_interval` seconds. Workers read it at scrape time. Internally named as `SelfMetrics`.
- **Route metrics**: per-route request counts, response-status classes, and bytes out, each tagged with the route's path and method.
- **Endpoint metrics**: per-outbound-endpoint request/completion counts, failure breakdowns, byte totals, and the live in-use slot gauge, each tagged with the endpoint's host.
- **Latency histograms** (optional): per-route and per-endpoint latency distributions, mapped only when `[Metrics] latency` is enabled in `wfx.toml`.

Route and endpoint counters are always available. Latency histograms are off by default; see [`[Metrics]`](../core_concepts/wfx_toml.md#metrics) for the memory/CPU trade-off and when to turn them on.

### Per-worker vs all workers

Each worker process tracks its own counters in its own slot. Slots are reached by
index, so a scrape can report every worker separately, and the `All` variants sum
them when you only want a total.

| Function                          | Returns                            |
|-----------------------------------|------------------------------------|
| `WFX::WorkerMetricCount()`        | Number of worker slots             |
| `WFX::WorkerIndex()`              | Index of the worker running this handler |
| `WFX::GetLogMetricsAt(w)`         | Worker `w`'s log counters          |
| `WFX::GetNetworkMetricsAt(w)`     | Worker `w`'s network counters      |
| `WFX::GetProcessMetricsAt(w)`     | Worker `w`'s process info          |
| `WFX::GetLogMetricsAll()`         | All workers summed                 |
| `WFX::GetNetworkMetricsAll()`     | All workers summed                 |
| `WFX::GetProcessMetricsAll()`     | All workers summed                 |

An index outside `[0, WorkerMetricCount())` returns a zeroed struct rather than
failing.

```cpp
// This worker's own numbers
const auto self = WFX::GetProcessMetricsAt(WFX::WorkerIndex());

// Per worker, so one sick worker stays visible instead of averaging away
for(std::uint16_t w = 0; w < WFX::WorkerMetricCount(); w++) {
    const auto net = WFX::GetNetworkMetricsAt(w);
    WFX::LogInfo("worker ", w, " accepts=", net.accepts, " bytesRead=", net.bytesRead);
}
```

!!! tip
    Prefer reporting per worker over `All` when you can. Summing hides the case
    where one worker is failing and the rest are healthy, which is exactly the
    situation worth catching.

### `LogMetrics` fields

```cpp
struct LogMetrics {
    std::uint64_t trace;
    std::uint64_t debug;
    std::uint64_t info;
    std::uint64_t warn;
    std::uint64_t error;
    std::uint64_t fatal;
};
```

### `NetworkMetrics` fields

```cpp
struct NetworkMetrics {
    std::uint64_t accepts;           // total accepted client connections
    std::uint64_t reads;             // total read syscalls that transferred data
    std::uint64_t bytesRead;         // total bytes received
    std::uint64_t writes;            // total write syscalls that transferred data
    std::uint64_t bytesWritten;      // total bytes sent via write
    std::uint64_t fileCalls;         // total successful file send syscalls
    std::uint64_t fileFallbacks;     // file sends that fell back to streaming (SSL only, Linux)
    std::uint64_t fileBytesWritten;  // total bytes sent via file sends
    std::uint64_t activeClientConns;   // inbound client connections currently open
    std::uint64_t activeEndpointConns; // outbound endpoint connections currently open
};
```

!!! note
    - `NetworkMetrics` is connection and syscall-level only. Request counts and
    response-status breakdowns are **not** here. They live in the per-route
    metrics (`RouteMetrics`), where they carry the path and method they belong to.
    See [Route metrics](#route-metrics) below.
    - `fileFallbacks` is incremented when a file send cannot use `sendfile` directly because the connection is SSL and kernel TLS is not available. In that case WFX falls back to streaming the file through the SSL write path in chunks. `fileCalls` and `fileBytesWritten` count both direct file sends and this fallback path.

### `SelfMetrics` fields

```cpp
struct SelfMetrics {
    std::uint64_t rssBytes;        // resident set size in bytes (physical memory in use)
    std::uint64_t vmBytes;         // virtual memory size in bytes
    std::uint32_t restarts;        // how many times this slot has been restarted
    std::uint32_t crashes;         // how many times this slot died unexpectedly
    std::uint32_t backoffAttempts; // current backoff attempt count
    std::int32_t  pid;             // current worker PID (-1 if dead)
    std::int64_t  startedAt;       // unix timestamp of last worker start
    std::int64_t  nextRetryAt;     // unix timestamp of next allowed restart (0 if not in backoff)
};
```

!!! important
    `GetProcessMetricsAll()` only meaningfully aggregates `rssBytes`, `vmBytes`, `restarts`, and `crashes`. Fields like `pid`, `startedAt`, `nextRetryAt`, and `backoffAttempts` are per-slot only and are zeroed out in the aggregate result.

### Route metrics

Every registered route gets its own slot, indexed densely from `0`. Routes are the home
for request counts and response-status classes: the counters carry the path and method
they belong to, so a scrape can attribute traffic per route instead of one server-wide
total.

| Function                       | Returns                                        |
|--------------------------------|------------------------------------------------|
| `WFX::RouteMetricCount()`      | Number of registered route slots               |
| `WFX::GetRouteMetricsAt(r)`    | Route `r`'s counters plus its path and method, summed across workers |

`GetRouteMetricsAt` returns a `RouteMetricsView`, which pairs the identity with the
counters:

```cpp
struct RouteMetricsView {
    Shared::StringView path;    // e.g. "/users/:id", a view valid for the process lifetime
    Shared::HttpMethod method;  // GET, POST, ...
    RouteMetrics       metrics;
};

struct RouteMetrics {
    std::uint64_t requests;   // requests served by this route
    std::uint64_t status1xx;  // responses with 1xx status
    std::uint64_t status2xx;  // responses with 2xx status
    std::uint64_t status3xx;  // responses with 3xx status
    std::uint64_t status4xx;  // responses with 4xx status
    std::uint64_t status5xx;  // responses with 5xx status
    std::uint64_t bytesOut;   // total response bytes written for this route
};
```

There is no per-worker route variant. `GetRouteMetricsAt` always sums across workers,
since a route is the same route in every worker. The counts are aggregated at read time.

```cpp
for(std::uint16_t r = 0; r < WFX::RouteMetricCount(); r++) {
    const auto rv = WFX::GetRouteMetricsAt(r);
    WFX::LogInfo(WFX::Shared::HttpMethodToStringView(rv.method), " ", rv.path,
                 " requests=", rv.metrics.requests, " 5xx=", rv.metrics.status5xx);
}
```

!!! note
    An index outside `[0, RouteMetricCount())` returns a zeroed view. There is no
    tracking for unmatched requests (404s that hit no route): they belong to no slot,
    so they contribute to no route's counters.

### Endpoint metrics

Outbound endpoints (the `WFX::HttpEndpoint` client and any other endpoint you declare)
get the same treatment: one slot per endpoint, indexed densely from `0`, each tagged
with the endpoint's host.

| Function                        | Returns                                          |
|---------------------------------|--------------------------------------------------|
| `WFX::EndpointMetricCount()`    | Number of registered endpoint slots              |
| `WFX::GetEndpointMetricsAt(e)`  | Endpoint `e`'s counters plus its host, summed across workers |

`GetEndpointMetricsAt` returns an `EndpointMetricsView`:

```cpp
struct EndpointMetricsView {
    Shared::StringView host;    // e.g. "api.example.com", valid for the process lifetime
    EndpointMetrics    metrics;
};

struct EndpointMetrics {
    std::uint64_t requests;        // requests issued to this endpoint
    std::uint64_t completed;       // requests that ran to a completed response
    std::uint64_t status1xx;       // completed responses with 1xx status
    std::uint64_t status2xx;       // completed responses with 2xx status
    std::uint64_t status3xx;       // completed responses with 3xx status
    std::uint64_t status4xx;       // completed responses with 4xx status
    std::uint64_t status5xx;       // completed responses with 5xx status
    std::uint64_t connectFailures; // could not establish a connection
    std::uint64_t tlsFailures;     // TLS handshake or client-wrap failures
    std::uint64_t requestTimeouts; // requests that exceeded the request timeout
    std::uint64_t poolExhausted;   // sends refused because the connection pool was full
    std::uint64_t otherErrors;     // everything else not split out above
    std::uint64_t reconnects;      // dead pooled connections re-established
    std::uint64_t coalesceHits;    // requests merged into an in-flight identical request
    std::uint64_t bytesOut;        // total request bytes written to this endpoint
    std::uint64_t bytesIn;         // total response bytes read from this endpoint
    std::uint64_t slotsInUse;      // gauge: connections currently leased against connLimit
};
```

`requests` counts every send; `completed` counts only the ones that produced a full
response. `completed` is also the denominator behind the latency mean below, so a
request that was coalesced (`coalesceHits`), timed out, or failed contributes no latency
sample. `slotsInUse` is a live gauge, not a monotonic counter.

The status counters are filled from the endpoint's `statusCode` hook, so they stay zero
for protocols that have no such concept.

```cpp
for(std::uint16_t e = 0; e < WFX::EndpointMetricCount(); e++) {
    const auto ev = WFX::GetEndpointMetricsAt(e);
    WFX::LogInfo("endpoint ", ev.host, " completed=", ev.metrics.completed,
                 " connectFailures=", ev.metrics.connectFailures,
                 " slotsInUse=", ev.metrics.slotsInUse);
}
```

### Latency histograms

When `latency = true` in the [`[Metrics]`](../core_concepts/wfx_toml.md#metrics) section,
WFX records a latency histogram per route and per endpoint. Both are off by default,
because each histogram is about 1.5 KB and recording costs two clock reads per request.
When the switch is off these getters return zeroed histograms.

| Function                          | Returns                                       |
|-----------------------------------|-----------------------------------------------|
| `WFX::MetricsLatencyEnabled()`    | Whether `[Metrics] latency` is on             |
| `WFX::GetRouteLatencyAt(r)`       | Route `r`'s latency histogram, summed across workers    |
| `WFX::GetEndpointLatencyAt(e)`    | Endpoint `e`'s latency histogram, summed across workers  |
| `WFX::ComputeLatencyStats(h)`     | Reduces a histogram to the derived percentiles and moments |

The raw `LatencyMetrics` is a running sum plus 192 buckets (24 power-of-two ranges of 8
linear sub-buckets, covering 1 microsecond to about 16 seconds). You rarely read it
directly. `ComputeLatencyStats` turns one into the numbers you actually want:

```cpp
struct LatencyStats {
    std::uint64_t count;    // number of recorded samples
    double        meanUs;   // exact mean (sumUs / count)
    double        stddevUs; // standard deviation, from bucket midpoints
    std::uint64_t minUs;
    std::uint64_t maxUs;
    std::uint64_t p50Us;
    std::uint64_t p90Us;
    std::uint64_t p95Us;
    std::uint64_t p99Us;
    std::uint64_t p999Us;
};
```

`count` and `meanUs` are exact. The percentiles, `minUs`, `maxUs`, and `stddevUs` are
read off the bucket midpoints, so they carry the histogram's own error (within 6.25% of
the true value at any bucket). `stddevUs` is midpoint-derived on purpose: an exact one
would need a stored sum of squares, which overflows a 64-bit accumulator at production
request volumes.

```cpp
if(WFX::MetricsLatencyEnabled()) {
    for(std::uint16_t r = 0; r < WFX::RouteMetricCount(); r++) {
        const auto rv = WFX::GetRouteMetricsAt(r);
        const auto st = WFX::ComputeLatencyStats(WFX::GetRouteLatencyAt(r));
        WFX::LogInfo(rv.path, " p50=", st.p50Us, "us p99=", st.p99Us,
                     "us max=", st.maxUs, "us");
    }
}
```

!!! note
    The latency sample for a route or endpoint is taken at exactly the point its
    `requests`/`completed` counter is incremented, so `meanUs` always equals
    `sumUs / completed`. Coalesced, timed-out, and failed requests are counted where
    appropriate but contribute no latency sample.

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
    j.End();

    // Request counts and status classes are per route, tagged with the path
    j.Arr("routes");
        for(std::uint16_t r = 0; r < WFX::RouteMetricCount(); r++) {
            const auto rv = WFX::GetRouteMetricsAt(r);
            j.Obj();
                j.Write("path",      rv.path);
                j.Write("requests",  rv.metrics.requests);
                j.Write("status_2xx", rv.metrics.status2xx);
                j.Write("status_4xx", rv.metrics.status4xx);
                j.Write("status_5xx", rv.metrics.status5xx);
                j.Write("bytes_out", rv.metrics.bytesOut);
            j.End();
        }
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
       .Write("# HELP wfx_requests_total Total HTTP requests per route\n")
       .Write("# TYPE wfx_requests_total counter\n");

    // Request and status-class counters are per route, labelled by path
    for(std::uint16_t r = 0; r < WFX::RouteMetricCount(); r++) {
        const auto rv = WFX::GetRouteMetricsAt(r);
        res.Write("wfx_requests_total{path=\"").Write(rv.path).Write("\"} ")
           .Write(rv.metrics.requests).Write("\n")
           .Write("wfx_responses_2xx_total{path=\"").Write(rv.path).Write("\"} ")
           .Write(rv.metrics.status2xx).Write("\n")
           .Write("wfx_responses_5xx_total{path=\"").Write(rv.path).Write("\"} ")
           .Write(rv.metrics.status5xx).Write("\n");
    }

    res.Write("wfx_active_client_connections ").Write(net.activeClientConns).Write("\n")
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