# WFX Settings

This page defines all supported configuration options for a `wfx.<env>.toml` file, one of which
lives at `config/wfx.<env>.toml` in your project for every environment you run or build against
(`config/wfx.local.toml` by default). See [Engine Commands](engine_commands.md#environments---env)
for how `--env` picks which file gets loaded.

!!! note
    - All settings in this page are applied **per worker process**, not globally. For example, if `max_connections` is set to `2000` and WFX is running with `4` worker processes, the effective maximum connection capacity is `2000 × 4 = 8000` concurrent connections.
    - If a setting does not explicitly mention `(In bytes)`, its value should be interpreted as a count, not a size. For example, `file_cache_size` represents the number of cached files, while `cache_chunk_size` and `template_chunk_size` explicitly specify `(In bytes)`, meaning their values are treated as byte sizes.

!!! warning
    WFX currently does **not validate value ranges or semantics**.
    It only checks for the **presence of certain required keys** (marked with `*`).
    If a required key is missing, startup fails.
    If a value is invalid but syntactically correct, behavior is **undefined and entirely the user's responsibility**.

---

## `[Project]`

Project-level configuration. This section is **mandatory**.

```toml
[Project]
middleware_list = [] # Array of strings
```

- `middleware_list`*: Ordered list of middleware identifiers.
    - Order matters: middleware executes exactly in declaration order
    - Middleware list may be empty, but the key itself must exist
    - Used to register both engine-provided and user-defined middleware

---

## `[Build]`

This section of the configuration file controls how WFX manages builds for your project.  
All values are **mandatory**.

```toml
[Build]
dir_name            = "build"          # String
preferred_config    = "Debug"          # String
preferred_generator = "Unix Makefiles" # String
```

- `dir_name`*  
  The folder where CMake will place all generated build files. This path is relative to the project folder.

- `preferred_config`*  
  The default build configuration (e.g., `"Debug"` for development builds or `"Release"` for optimized production builds).  

- `preferred_generator`*  
  The default CMake generator to use (e.g., `Unix Makefiles`, `Ninja`).  

---

## `[Network]`

Connection-level configuration. All values are **optional**; defaults are applied if omitted.

```toml
[Network]
send_buffer_max              = 16384   # 32-bit Unsigned Integer (In bytes)
send_buffer_incr             = 4096    # 32-bit Unsigned Integer (In bytes)
recv_buffer_max              = 16384   # 32-bit Unsigned Integer (In bytes)
recv_buffer_incr             = 4096    # 32-bit Unsigned Integer (In bytes)
header_reserve_hint          = 512     # 16-bit Unsigned Integer (In bytes)
max_header_size              = 8192    # 32-bit Unsigned Integer (In bytes)
max_body_size                = 8192    # 32-bit Unsigned Integer (In bytes)
max_header_count             = 64      # 16-bit Unsigned Integer
header_timeout               = 15      # 16-bit Unsigned Integer (In seconds)
body_timeout                 = 20      # 16-bit Unsigned Integer (In seconds)
idle_timeout                 = 60      # 16-bit Unsigned Integer (In seconds)
max_connections              = 2000    # 32-bit Unsigned Integer
```

### Buffers

- `send_buffer_max`: Max total outbound buffer per connection
- `send_buffer_incr`: Growth increment when send buffer expands
- `recv_buffer_max`: Max total inbound buffer per connection
- `recv_buffer_incr`: Growth increment when receive buffer expands
- `header_reserve_hint`: Initial allocation hint for headers

### Headers & Body

- `max_header_size`: Max combined size of all headers
- `max_header_count`: Max number of headers allowed
- `max_body_size`: Max request body size

### Timeouts

- `header_timeout`: Time allowed to fully receive headers
- `body_timeout`: Time allowed to fully receive body
- `idle_timeout`: Max idle time before connection is closed

### Connections

- `max_connections`  
  Maximum number of simultaneous connections handled by a single worker process.  
  Internally, WFX rounds this value **up to the nearest multiple of 64** for efficiency.  
  This is a hard cap; once reached, new connections are rejected by that worker.

---

## `[ENV]`

Environment variable loading. This section is **optional**.

```toml
[ENV]
env_path = "..." # String (Path to .env file)
```

!!! note
    On non-Windows systems, the file must have permission `600` (meaning that only the file's owner has read and write access).
    Insecure permissions may result in startup failure.

---

## `[IP]`

Per-IP connection/request limiting and real-client-IP resolution. All values are **optional**;
defaults are applied if omitted.

```toml
[IP]
max_connections_per_ip       = 20      # 32-bit Unsigned Integer
max_request_burst_per_ip     = 10      # 32-bit Unsigned Integer
max_requests_per_ip_per_sec  = 5       # 32-bit Unsigned Integer
max_tracked_identities       = 24576   # 32-bit Unsigned Integer
real_ip_header               = ""      # String (Header name, or "" to always use the raw peer IP)
real_ip_recursive            = false   # Boolean (true or false)
trusted_proxies              = []      # Array of Strings (CIDR blocks)
```

### Limiting

- `max_connections_per_ip`  
  Maximum number of simultaneous connections allowed from a single IP address per worker process.  
  This prevents one client from consuming all available connections.

- `max_request_burst_per_ip`  
  The number of requests an IP address is allowed to send immediately without being throttled.  
  Think of this as a bucket of tokens given to each IP when it first connects.

- `max_requests_per_ip_per_sec`  
  How fast the token bucket for each IP is refilled, measured in **tokens per second**.  
  Once an IP runs out of tokens, further requests are delayed or rejected until tokens are refilled.

- `max_tracked_identities`  
  Maximum number of distinct resolved identities whose rate-limit bucket is kept in memory at
  once, per worker process. Unlike the connection cap, a rate-limit bucket has to survive its
  owning connection closing (see `max_requests_per_ip_per_sec`), so this is what bounds its
  memory instead: once the cap is reached, the least-recently-seen identity is evicted to make
  room for a new one, never one still tied to an open connection.  
  **Always rounded up to a multiple of 64 internally, so the real minimum is 64** regardless of
  the configured value.

### Real IP

- `real_ip_header`  
  Name of a request header to trust for the real client IP, e.g. `"CF-Connecting-IP"` behind
  Cloudflare, or `"X-Forwarded-For"` behind a generic reverse proxy. Left empty (the default),
  every limiter always uses the raw peer IP, no header is ever consulted.

- `real_ip_recursive`  
  Only relevant for header values that can hold a comma-separated chain, such as
  `X-Forwarded-For`. When `true`, WFX walks the chain right-to-left, skipping entries that are
  themselves trusted proxies, until it finds the first one that isn't. When `false` (the
  default), the header's value is used as-is.

- `trusted_proxies`  
  List of CIDR blocks (e.g. `"173.245.48.0/20"`) allowed to set `real_ip_header`. A request whose
  peer IP isn't inside one of these blocks has its header ignored and falls back to the raw peer
  IP, even if `real_ip_header` is configured, this is what stops a client from spoofing its own
  IP by just setting the header itself. An empty list (the default) means nothing matches, so
  `real_ip_header` is effectively never honored until at least one block is added.

---

## `[CORS]`

Cross-Origin Resource Sharing. This section is **optional**; CORS is off entirely when omitted or
`enabled = false`.

```toml
[CORS]
enabled           = false  # Boolean
allowed_origins   = []     # Array of Strings
allowed_methods   = "GET, POST, PUT, PATCH, DELETE, OPTIONS" # String
allowed_headers   = []     # Array of Strings
exposed_headers   = []     # Array of Strings
allow_credentials = false  # Boolean
max_age           = 600    # 32-bit Unsigned Integer (In seconds)
```

- `enabled`  
  Turns CORS handling on or off. When `false`, none of the other settings in this section matter,
  no CORS headers are ever written and no request is treated as a preflight.

- `allowed_origins`  
  Exact origins allowed to make cross-origin requests, e.g. `["https://example.com"]`. Matching is
  exact string comparison, not a prefix, suffix, or subdomain match, `https://evil.example.com`
  does **not** match an allowlisted `https://example.com`, and neither does `http://example.com`
  (different scheme) or `https://example.com:8443` (different port). A single `"*"` entry allows
  any origin. `"*"` cannot be combined with `allow_credentials = true`, WFX refuses to start if
  you configure that combination, browsers refuse to honor it anyway.

- `allowed_methods`  
  The exact value sent back as `Access-Control-Allow-Methods` on a preflight response. This is a
  static, comma-separated string, not derived from what routes you've actually registered, every
  preflight gets this same list regardless of which path it's for.

- `allowed_headers`  
  Request headers a cross-origin caller is allowed to send. Left empty (the default), WFX reflects
  whatever the preflight's own `Access-Control-Request-Headers` asked for, which works for most
  setups without listing every custom header by hand. Set it explicitly to lock the response down
  to a fixed list instead of trusting whatever the browser requested.

- `exposed_headers`  
  Response headers JavaScript is allowed to read beyond the small set browsers always allow
  (`Cache-Control`, `Content-Language`, `Content-Length`, `Content-Type`, `Expires`,
  `Last-Modified`, `Pragma`). Left empty (the default), `Access-Control-Expose-Headers` is omitted
  entirely.

- `allow_credentials`  
  Sends `Access-Control-Allow-Credentials: true`, letting a cross-origin request include cookies
  or an `Authorization` header and letting the browser expose the response back to the page.
  Cannot be combined with a `"*"` entry in `allowed_origins`, see above.

- `max_age`  
  How long, in seconds, a browser is allowed to cache a preflight response before sending a new
  one for the same origin/method/headers combination. Browsers cap this regardless of what's
  sent: Chrome to 7200s, Firefox to 86400s, Safari to 300s, so setting it higher than a browser's
  own cap has no effect on that browser.

!!! note "Preflight is answered before your handler ever runs"
    A real preflight (an `OPTIONS` request carrying `Access-Control-Request-Method`) is answered
    entirely by the engine, before routing and before any middleware (it still goes through
    connection/rate limiting first, same as every other request). Your route handlers, including
    one registered with `WFX_OPTIONS` at the same path, never see it. See
    [Routing](../api_reference/routing.md#cors-and-options) for the full behavior, including what
    happens to bare `OPTIONS` requests that aren't a CORS preflight at all.

---

## `[SSL]`

TLS configuration. This section is **only used when WFX is running in HTTPS mode**.
When HTTPS is enabled, certificate paths are **mandatory**; all other settings are **optional**.

```toml
[SSL]
cert_path                   = "..."           # String (Path to server certificate)
key_path                    = "..."           # String (Path to private key)
outbound_ca_path            = ""              # String (Path to an extra trusted CA, or "" to use the system store)
client_ca_path              = ""              # String (Path to a CA for verifying inbound client certs, or "" to disable mTLS)
tls13_ciphers               = "..."           # String
tls12_ciphers               = "..."           # String
curves                      = "X25519:P-256"  # String
enable_server_session_cache = true            # Boolean (true or false)
enable_client_session_cache = true            # Boolean (true or false)
enable_ktls                 = false           # Boolean (true or false)
server_session_cache_size   = 4096            # 64-bit Unsigned Integer (In bytes)
client_session_cache_size   = 1024            # 64-bit Unsigned Integer (In bytes)
min_proto_version           = 2               # 8-bit Unsigned Integer (1 - 3 only)
security_level              = 2               # Integer (0 - 5 only)
```

### Certificates

- `cert_path`*  
  PEM-encoded server certificate, presented to inbound HTTPS clients.
- `key_path`*  
  Private key matching `cert_path`.
- `outbound_ca_path`  
  Path to an extra CA certificate that WFX should trust when it connects out to other servers, on top of what your operating system already trusts. Use this if a server you are connecting to presents a certificate signed by an internal or self signed CA, for example a certificate used for local testing, since your OS would not already trust it. Leave it empty to rely only on your system trust store, which is the right choice for most public servers. This setting only affects connections WFX makes outward, it does not change how WFX verifies clients connecting to it.

### Client Certificates (mTLS)

- `client_ca_path`  
  Path to a CA certificate WFX uses to verify certificates presented by connecting clients. Leaving this empty disables mutual TLS entirely, clients connect exactly as they do today, no certificate requested. Setting it to a non-empty path does two things at once: it starts requiring every inbound client to present a certificate, and rejects the handshake if that certificate wasn't signed by this CA. There is no separate on/off setting, presence of `client_ca_path` is what turns mTLS on. This is unrelated to `outbound_ca_path`, which only affects connections WFX itself makes outward as a client; this setting only affects how WFX verifies clients connecting to it.

### Cipher Suites

- `tls13_ciphers`  
  This lists the preferred encryption methods for TLS 1.3 connections, separated by colons.  
  **Example**: `"TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256"` tells WFX to first try `TLS_AES_128_GCM_SHA256` with the client, and only if the client doesn't support it, it will fall back to `TLS_CHACHA20_POLY1305_SHA256`.

- `tls12_ciphers`  
  Same as above but for TLS 1.2 connections. These ciphers define how the server and client encrypt and verify data during the handshake.  
  **Example**: `"ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384"`

- `curves`  
  Determines the order of Elliptic Curve Diffie-Hellman (ECDHE) curves used for key exchange.  
  **Example**: `"X25519:P-256"` tells WFX to try X25519 first, then P-256. This affects speed and security of the handshake.

### TLS Behavior

- `enable_server_session_cache`  
  When enabled, WFX caches TLS session state for **inbound** HTTPS clients so a
  returning client can resume a session instead of doing a full handshake.  
  **Example**: a returning client skips the expensive key exchange, improving speed at the cost of more RAM usage.

- `enable_client_session_cache`  
  Same idea, but for the **outbound** `HttpEndpoint` TLS client: lets WFX resume
  a session when reconnecting to the same upstream (e.g. after an idle-timeout
  disconnect or a pool reconnect), instead of doing a full handshake every time.

- `server_session_cache_size` / `client_session_cache_size`  
  Maximum memory allocated for each of the two session caches above. When a
  limit is reached, older sessions are evicted, and the next reconnect on that
  side performs a full TLS handshake again.

- `enable_ktls`  
  Uses Kernel TLS, which offloads encryption tasks to the OS kernel for higher performance. Older versions of kernel may not fully support this feature.

### Protocol & Security

- `min_proto_version`  
  Sets the minimum TLS version allowed.  
  **Example**: `2` means TLS 1.2 or higher only; older clients using TLS 1.0 or 1.1 will be rejected for security reasons.

- `security_level`  
  OpenSSL security strictness (0-5). Higher values enforce stronger algorithms, longer keys, and stricter certificate checks.  
  **Example**: `2` is a reasonable default, while `5` is extremely strict and may block older clients.

---

## `[Linux]`

Socket and worker configuration for **Linux systems only**. All settings in this section are **optional**.

```toml
[Linux]
worker_processes        = 4     # 32-bit Unsigned Integer
worker_shutdown_timeout = 5     # 16-bit Unsigned Integer (In seconds)
backlog                 = 1024  # 32-bit Unsigned Integer
```

- `worker_processes`  
  Controls how many worker processes WFX starts to handle incoming requests. More workers allow better CPU usage on multi-core systems, but too many can waste memory or cause contention.  
  **Guidance**: Start with significantly fewer workers than total CPU cores, leaving ample headroom for the OS, networking, TLS, and background tasks. Increase gradually only after load testing shows CPU saturation.

- `worker_shutdown_timeout`  
  Seconds to wait for workers to exit cleanly after receiving SIGTERM before force-killing whichever ones haven't. This is one shared window covering all workers together, not per worker, so shutdown time doesn't grow with `worker_processes`.

- `backlog`  
  Sets the maximum number of incoming connections the OS can queue while workers are busy. If this limit is too low, new connections may be rejected during traffic spikes even if the server is healthy.

## `[Linux.Epoll]`

This is the default Linux networking backend. All settings in this section are **optional**.

```toml
[Linux.Epoll]
max_events = 1024 # 16-bit Unsigned Integer
```

- `max_events`  
  Defines how many I/O events `epoll_wait` can return at once. Higher values allow the server to process more ready connections per loop, while lower values reduce per-iteration work but may increase latency under load.

---

## `[Logging]`

Controls how WFX emits log output. This section is **optional**.

```toml
[Logging]
min_level         = 2         # 8-bit Unsigned Integer (0 to 5)
enable_stdout     = true      # Boolean
enable_colors     = true      # Boolean
enable_timestamps = true      # Boolean
enable_file       = false     # Boolean
max_file_size     = 16777216  # 32-bit Unsigned Integer (In bytes)
max_rotations     = 2         # 16-bit Unsigned Integer
```

- `min_level`: Minimum log level to emit. `0` = trace, `1` = debug, `2` = info, `3` = warn, `4` = error, `5` = fatal. Lines below this level are discarded entirely.
- `enable_stdout`: Write log output to stdout.
- `enable_colors`: ANSI color codes on stdout. Automatically disabled if stdout is not a TTY.
- `enable_timestamps`: Prepend `[HH:MM:SS.mmm]` to each log line.
- `enable_file`: Write log output to per-worker log files under `logs/default_logs/`.
- `max_file_size`: Max size of a single log file before it rotates. Only applies when `enable_file = true`.
- `max_rotations`: Number of rotated log files to keep. Files are named `.1` through `.N`, oldest are discarded. Only applies when `enable_file = true`.

---

## `[Metrics]`

Controls the per-route and per-endpoint metrics tables and optional latency histograms. This section is **optional**.

```toml
[Metrics]
max_routes    = 256    # 16-bit Unsigned Integer
max_endpoints = 256    # 16-bit Unsigned Integer
latency       = false  # Boolean
```

- `max_routes`: Number of route slots reserved in the per-route metrics table. Routes are indexed densely from `0`, so this caps how many distinct routes can be tracked. Registering more routes than this leaves the overflow untracked.
- `max_endpoints`: Number of endpoint slots reserved in the per-endpoint metrics table, indexed the same way as routes.
- `latency`: Record per-route and per-endpoint latency histograms. When `true`, each request costs two extra clock reads and each tracked route/endpoint gets its own histogram in the shared metrics map. Leave it `false` in normal operation and enable it only when profiling. When `false`, `WFX::GetRouteLatencyAt` / `WFX::GetEndpointLatencyAt` return zeroed histograms and `WFX::MetricsLatencyEnabled()` returns `false`.

!!! note "Allocation is lazy"
    `max_routes` and `max_endpoints` reserve **virtual** address space in the shared metrics map, not physical memory. The map is anonymous, so a page only faults into physical memory the first time a slot on it is actually written. A slot is written when its route/endpoint first serves a request, so setting these higher than you need costs address space but almost no real memory. Size them to your worst case and forget about them; the counter tables themselves are tiny (roughly 48 KB per worker at the defaults).

!!! warning "When to enable `latency`"
    Latency histograms are the one part of the metrics map that is genuinely expensive. Each histogram is ~1.5 KB, and turning `latency` on maps one per route **and** one per endpoint, per worker. At the defaults that is ~772 KB of reserved space per worker (still lazily faulted, so real cost tracks the slots you actually touch), on top of two clock reads on every request.

    Keep `latency = false` for steady-state production, where the counter tables (request counts, status classes, byte totals, endpoint failures) already answer "what is happening." Turn it on when you specifically need the shape of the latency distribution: chasing a p99 regression, validating a change under load, or capacity planning. Turn it back off when you are done. It is a profiling switch, not an always-on gauge.

See [Telemetry](../api_reference/telemetry.md) for the read-side API that consumes these tables.

---

## `[Misc]`

Miscellaneous engine-level settings covering caching, internal I/O, and worker process management. This section is **optional**.

```toml
[Misc]
file_cache_size      = 20    # 16-bit Unsigned Integer
cache_chunk_size     = 2048  # 16-bit Unsigned Integer (In bytes)
template_chunk_size  = 16384 # 32-bit Unsigned Integer (In bytes)
master_poll_interval = 2     # 16-bit Unsigned Integer (In seconds)
max_worker_restarts  = 5     # 16-bit Unsigned Integer
worker_backoff_base  = 1     # 16-bit Unsigned Integer (In seconds)
worker_backoff_max   = 16    # 16-bit Unsigned Integer (In seconds)
```

- `file_cache_size`: Number of files cached in memory (LFU)
- `template_chunk_size`: Max I/O chunk size during template compilation
- `cache_chunk_size`: Max I/O chunk size for template cache files
- `master_poll_interval`: How often the master process wakes up to check for dead workers and poll memory metrics. Lower values mean faster crash detection at the cost of slightly more wakeups.
- `max_worker_restarts`: Maximum number of times a crashed worker slot will be restarted before it is marked permanently dead until the server restarts.
- `worker_backoff_base`: Starting backoff delay in seconds before the first restart attempt. Doubles on each subsequent attempt.
- `worker_backoff_max`: Maximum backoff delay cap in seconds. Backoff will never exceed this value regardless of how many attempts have occurred.