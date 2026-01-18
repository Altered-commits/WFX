# WFX Settings

This page defines all supported `wfx.toml` configuration options in WFX.

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

<pre class="code-format">
[Project]
middleware_list = [] # Array of strings
</pre>

- `middleware_list`*: Ordered list of middleware identifiers.
    - Order matters: middleware executes exactly in declaration order
    - Middleware list may be empty, but the key itself must exist
    - Used to register both engine-provided and user-defined middleware

---

## `[Build]`

This section of the configuration file controls how WFX manages builds for your project.  
All values are **mandatory**.

<pre class="code-format">
[Build]
dir_name            = "build"          # String
preferred_config    = "Debug"          # String
preferred_generator = "Unix Makefiles" # String
</pre>

- `dir_name`*  
  The folder where CMake will place all generated build files. This path is relative to the project folder.

- `preferred_config`*  
  The default build configuration (e.g., `"Debug"` for development builds or `"Release"` for optimized production builds).  

- `preferred_generator`*  
  The default CMake generator to use (e.g., `Unix Makefiles`, `Ninja`).  

---

## `[Network]`

Connection-level configuration. All values are **optional**; defaults are applied if omitted.

<pre class="code-format">
[Network]
send_buffer_max              = 2048    # 32-bit Unsigned Integer (In bytes)
recv_buffer_max              = 16384   # 32-bit Unsigned Integer (In bytes)
recv_buffer_incr             = 4096    # 32-bit Unsigned Integer (In bytes)
header_reserve_hint          = 512     # 16-bit Unsigned Integer (In bytes)
max_header_size              = 8192    # 32-bit Unsigned Integer (In bytes)
max_body_size                = 8192    # 32-bit Unsigned Integer (In bytes)
max_header_count             = 64      # 16-bit Unsigned Integer
header_timeout               = 15      # 16-bit Unsigned Integer (In seconds)
body_timeout                 = 20      # 16-bit Unsigned Integer (In seconds)
idle_timeout                 = 40      # 16-bit Unsigned Integer (In seconds)
max_connections              = 2000    # 32-bit Unsigned Integer
max_connections_per_ip       = 20      # 32-bit Unsigned Integer
max_request_burst_per_ip     = 10      # 32-bit Unsigned Integer
max_requests_per_ip_per_sec  = 5       # 32-bit Unsigned Integer
</pre>

### Buffers

- `send_buffer_max`: Max total outbound buffer per connection
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

- `max_connections_per_ip`  
  Maximum number of simultaneous connections allowed from a single IP address per worker process.  
  This prevents one client from consuming all available connections.

- `max_request_burst_per_ip`  
  The number of requests an IP address is allowed to send immediately without being throttled.  
  Think of this as a bucket of tokens given to each IP when it first connects.

- `max_requests_per_ip_per_sec`  
  How fast the token bucket for each IP is refilled, measured in **tokens per second**.  
  Once an IP runs out of tokens, further requests are delayed or rejected until tokens are refilled.

---

## `[ENV]`

Environment variable loading. This section is **optional**.

<pre class="code-format">
[ENV]
env_path = "..." # String (Path to .env file)
</pre>

!!! note
    On non-Windows systems, the file must have permission `600` (meaning that only the file's owner has read and write access).
    Insecure permissions may result in startup failure.

---

## `[SSL]`

TLS configuration. This section is **only used when WFX is running in HTTPS mode**.
When HTTPS is enabled, certificate paths are **mandatory**; all other settings are **optional**.

<pre class="code-format">
[SSL]
cert_path            = "..."           # String (Path to certificate)
key_path             = "..."           # String (Path to private key)
tls13_ciphers        = "..."           # String
tls12_ciphers        = "..."           # String
curves               = "X25519:P-256"  # String
enable_session_cache = true            # Boolean (true or false)
enable_ktls          = false           # Boolean (true or false)
session_cache_size   = 32768           # 64-bit Unsigned Integer (In bytes)
min_proto_version    = 3               # 8-bit Unsigned Integer (1 - 3 only)
security_level       = 2               # Integer (0 - 5 only)
</pre>

### Certificates

- `cert_path`*: PEM-encoded server certificate
- `key_path`*: Private key matching the certificate

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

- `enable_session_cache`  
  When enabled, WFX stores TLS session information on the server so clients can reconnect faster without doing a full handshake.  
  **Example**: a returning client can skip the expensive key exchange, improving speed at the cost of more RAM usage.

- `session_cache_size`  
  Maximum memory size allocated for caching TLS session data. When this limit is reached, older sessions are evicted, causing returning clients to perform a full TLS handshake again.

- `enable_ktls`  
  Uses Kernel TLS, which offloads encryption tasks to the OS kernel for higher performance. Older versions of kernel may not fully support this feature.

### Protocol & Security

- `min_proto_version`  
  Sets the minimum TLS version allowed.  
  **Example**: `2` means TLS 1.2 or higher only; older clients using TLS 1.0 or 1.1 will be rejected for security reasons.

- `security_level`  
  OpenSSL security strictness (0–5). Higher values enforce stronger algorithms, longer keys, and stricter certificate checks.  
  **Example**: `2` is a reasonable default, while `5` is extremely strict and may block older clients.

---

## `[Windows]`

!!! note
    This section will be updated once official Windows support is released.

---

## `[Linux]`

Socket and worker configuration for **Linux systems only**. All settings in this section are **optional**.

<pre class="code-format">
[Linux]
worker_processes = 2     # 32-bit Unsigned Integer
backlog          = 1024  # 32-bit Unsigned Integer
</pre>

- `worker_processes`  
  Controls how many worker processes WFX starts to handle incoming requests. More workers allow better CPU usage on multi-core systems, but too many can waste memory or cause contention.  
  **Guidance**: Start with significantly fewer workers than total CPU cores, leaving ample headroom for the OS, networking, TLS, and background tasks. Increase gradually only after load testing shows CPU saturation.

- `backlog`  
  Sets the maximum number of incoming connections the OS can queue while workers are busy. If this limit is too low, new connections may be rejected during traffic spikes even if the server is healthy.

## `[Linux.IoUring]`

!!! note
    This section will be updated once official IoUring support is released.

## `[Linux.Epoll]`

This is the default Linux networking backend. All settings in this section are **optional**.

<pre class="code-format">
[Linux.Epoll]
max_events = 1024 # 16-bit Unsigned Integer
</pre>

- `max_events`  
  Defines how many I/O events `epoll_wait` can return at once. Higher values allow the server to process more ready connections per loop, while lower values reduce per-iteration work but may increase latency under load.

---

## `[Misc]`

Defines small engine-level limits that do not fit into other categories, mainly related to caching and internal I/O behavior. This section is **optional**.

<pre class="code-format">
[Misc]
file_cache_size     = 20     # 16-bit Unsigned Integer
cache_chunk_size    = 2048   # 16-bit Unsigned Integer (In bytes)
template_chunk_size = 16384  # 32-bit Unsigned Integer (In bytes)
</pre>

- `file_cache_size`: Number of files cached in memory (LFU)
- `template_chunk_size`: Max I/O chunk size during template compilation
- `cache_chunk_size`: Max I/O chunk size for template cache files