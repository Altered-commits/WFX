# Endpoint

Endpoints are WFX's way of talking to **other servers** from inside a route handler:
a REST API, a database, a cache, anything reachable over TCP. An endpoint owns a
pooled set of outbound connections to one host, and route handlers borrow a
connection from that pool with `co_await`, exactly the same way they `co_await`
anything else in WFX's async system.

There are two layers:

- **`WFX::Endpoint<TReq, TRes>`**, documented on this page, is the raw, protocol-agnostic
  primitive. It owns the connection pool, DNS resolution, reconnects, TLS, timeouts, and
  coalescing. You provide the wire format for one protocol (how to serialize a request and
  parse a response), and everything else is handled for you.
- **[`WFX::HttpEndpoint`](http.md)** is a ready-to-use HTTP/1.1 client built on top of this
  primitive. If you're calling an HTTP upstream, use that page instead, it covers the same
  ground as this page but pre-wired for HTTP.

Everything on this page applies to **any** protocol built on `Endpoint<>`, current or future
(Redis, Postgres, a custom binary protocol, `HttpEndpoint` itself). A protocol-specific doc
page only needs to describe its own wire format and request/response shape; the pool,
lifecycle, DNS, TLS, and coalescing behavior described here is shared by all of them.

!!! important
    All endpoint functionality lives inside the `WFX::` namespace. To build a protocol
    client on the raw primitive, include:
    ```cpp
    #include <wfx/endpoint/base.hpp>
    ```

---

## Declaring an endpoint

An endpoint is declared once, at namespace scope, before `Run()`:

```cpp
inline const auto Api = WFX::Endpoint<ApiReq, ApiRes>{
    "api.example.com:80",
    WFX::EndpointDesc{ .serialize = Serialize, .parse = Parse },
    WFX::EndpointConfig{ .connLimit = 4, .requestTimeoutSeconds = 10 }
};
```

This single declaration represents **one upstream host** and **one connection pool**
for that host. Every route handler that talks to that host through `Api` shares
the same pool.

!!! important
    Declare endpoints as `inline const auto` at namespace scope, the same way you
    declare routes. Registration happens once, before the server starts accepting
    connections. DNS resolution for the host also happens at that point, so if the
    hostname cannot be resolved, the server refuses to start rather than fail later
    at request time.

    Each worker process keeps its own copy of the pool. `connLimit` and every other
    pool setting is a **per-worker** limit, not a total shared across all workers.

The host string must be in `"hostname:port"` form, with no scheme prefix. A scheme
prefix (`"https://..."`) or a missing port is a fatal startup error, not a runtime one.

TLS is chosen automatically based on the port unless you override it, see
[TLS mode](#tls-mode) below.

---

## Sending a request

Every send goes through `co_await` and returns a pair: a status code, and an RAII
handle to the response.

```cpp
WFX_GET("/proxy", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    auto [status, out] = co_await Api.SendPayload({"/users"});

    if(status != WFX::EpOk) {
        res.Status(502).SendText("upstream error");
        co_return;
    }

    res.Status(out->status).SendText(out->body);
    co_return;
});
```

`out` is a `WFX::EndpointOutput<T>`. Access the underlying response with `*out`,
`out->`, or `out.get()`. It stays valid for as long as the variable is in scope,
and frees the response automatically when it goes out of scope. Do not store the
raw pointer past that point.

### Status codes

`status` (the first element of the pair) tells you what happened. Always check it
before touching `out`, a non-`EpOk` status means `out` is empty.

| Status | Meaning |
|--------|---------|
| `WFX::EpOk` | Success, `out` is valid |
| `WFX::EpPoolExhausted` | All connections to this host are busy, try again later |
| `WFX::EpSocketFailure` | Local socket creation or configuration failed, before ever reaching `connect()` |
| `WFX::EpConnectFailure` | `connect()` itself failed |
| `WFX::EpSslFailure` | TLS handshake failed |
| `WFX::EpHandshakeTimeout` | TCP connect, TLS handshake, or `onConnect` took too long |
| `WFX::EpRequestTimeout` | No response within `requestTimeoutSeconds` |
| `WFX::EpSerializeError` | Your `serialize()` returned an error |
| `WFX::EpBufferError` | Internal buffer setup failed |
| `WFX::EpInsufficientBuffer` | Request too large even after the engine grew the buffer |
| `WFX::EpInvalidKey` | Endpoint index out of range (an engine bug, not a caller mistake) |
| `WFX::EpEpollError` | Re-arming epoll on a reused connection failed |
| `WFX::EpInternalError` | Unclassified engine failure |

!!! important "What you can actually expect back"
    Not every value above arrives the same way. Two of them are only ever
    produced through a specific path, worth knowing so you don't write handling
    logic assuming a value can appear where it can't:

    - `WFX::EpOk`, the happy path.
    - `WFX::EpPoolExhausted`, `WFX::EpSocketFailure`, `WFX::EpConnectFailure`,
      `WFX::EpSslFailure`, `WFX::EpBufferError`, `WFX::EpInsufficientBuffer`,
      `WFX::EpSerializeError`, `WFX::EpEpollError`, returned immediately,
      without suspending, when the request could not even be started.
    - `WFX::EpHandshakeTimeout`, `WFX::EpRequestTimeout`, and
      `WFX::EpInternalError`, returned after suspending, once the connection
      was already being established or already in flight. A timeout while
      still connecting or handshaking reports as `EpHandshakeTimeout`, a
      timeout once a request was already sent reports as `EpRequestTimeout`,
      and every other mid-flight failure (a reset, a parse error, a reconnect
      budget running out, a coalesce primary dying) reports as
      `EpInternalError`.
    - `WFX::EpInvalidKey` exists for completeness but signals an engine
      registration bug, not something a caller can trigger.

    Write your `if(status != WFX::EpOk)` fallback to treat any status you
    don't explicitly handle the same way, rather than enumerating every value
    and hoping the list stays exhaustive.

---

## Connection pool & lifecycle

Every endpoint owns a fixed-size pool of connection slots, sized by `connLimit`.
A slot moves through roughly this lifecycle:

1. **Closed** -> no socket, nothing allocated.
2. **Connecting** -> TCP connect (and TLS handshake, if applicable) in progress.
3. **`onConnect`** -> only if the protocol defines one; a handshake coroutine runs before the slot is usable, see [below](#onconnect-handshakes).
4. **In the pool, idle** -> connected and healthy, waiting to be leased for a request.
5. **Leased** -> actively serving one caller's request.
6. Back to **idle** on a keep-alive-friendly completion, or **closed** if the protocol asked to close, the peer closed, or something failed.

### Pool settings

```cpp
struct EndpointConfig {
    std::uint32_t connLimit;
    std::uint32_t dnsRefreshSeconds;
    std::uint16_t connectTimeoutSeconds;
    std::uint16_t requestTimeoutSeconds;
    std::uint32_t idleTimeoutSeconds;
    std::uint16_t maxReconnectAttempts;
    std::uint16_t reconnectBackoffBase;
    std::uint16_t reconnectBackoffMax;
    EndpointTLSConfig tlsConfig;
    std::uint32_t prewarm;
    std::uint32_t maxConcurrentStreams; // multiplexing, see below. 0/1 = exclusive slot
    StringView alpnProtocols;           // wire-encoded ALPN list, empty offers nothing extra
};
```

| Setting | Meaning |
|---------|---------|
| `connLimit` | Max simultaneous connections to this host, per worker |
| `dnsRefreshSeconds` | `0` to follow the DNS record's own TTL, or a ceiling in seconds |
| `connectTimeoutSeconds` | Budget for TCP connect + TLS handshake + `onConnect` combined |
| `requestTimeoutSeconds` | Budget for one send + receive cycle once a request starts |
| `idleTimeoutSeconds` | Idle pooled connections close after this many seconds of inactivity |
| `maxReconnectAttempts` | How many times a background connection retries before being ejected |
| `reconnectBackoffBase` / `reconnectBackoffMax` | Backoff range (seconds) between reconnect attempts |
| `tlsConfig` | TLS mode, see [below](#tls-mode) |
| `prewarm` | Connections to open eagerly on startup |
| `maxConcurrentStreams` | Cap on requests sharing one slot, see [Multiplexing](#multiplexing-advanced) |
| `alpnProtocols` | ALPN protocols to offer during the TLS handshake |

!!! important
    These are validated at startup and the server refuses to start if any are
    invalid: `connLimit` must be greater than zero, `prewarm` cannot exceed
    `connLimit`, `reconnectBackoffBase` cannot exceed `reconnectBackoffMax`, and
    every timeout must be at least as long as the engine's internal timer tick
    (5 seconds). A shorter value would silently fire later than configured, so
    WFX rejects it outright instead of lying to you.

### Reconnects and backoff

If a connection fails while nothing is waiting on it (a prewarmed or idle slot
losing its socket, for example), WFX retries it in the background with
exponential backoff plus jitter (`base * 2^attempt`, capped at
`reconnectBackoffMax`, then randomized so a whole pool doesn't reconnect in
lockstep). Once `maxReconnectAttempts` is exhausted, the slot is permanently
ejected and freed.

If a caller is actively waiting on that connection when it fails, WFX does not
make it wait through a backoff delay. The failure is reported immediately
through the status code, so your route handler can decide whether to retry.

### Prewarm

Setting `prewarm` opens that many connections eagerly before the server starts
accepting traffic, instead of waiting for the first request to pay the connect
cost. Prewarm failures go through the same reconnect funnel as any other
background connection.

---

## DNS resolution & refresh

The host is resolved once, synchronously, when the endpoint is registered.
Resolution failure at that point is fatal, the server will not start with an
endpoint pointed at a host it cannot resolve.

Each time a new physical connection is opened, WFX picks the next address from
the resolved set in round-robin order.

Addresses are periodically re-resolved in the background so a changing DNS
record (a failover, a load balancer swap) is eventually picked up without
restarting the server:

- With `dnsRefreshSeconds = 0` (the default), the refresh interval follows the
  DNS record's own TTL.
- With `dnsRefreshSeconds` set to a positive value, that value acts as a
  ceiling: WFX refreshes at most that often, but never waits *longer* than the
  record's real TTL if the TTL is shorter.
- The interval is always clamped between 5 seconds and 1 hour, and a small
  amount of jitter is added so many endpoints don't all refresh at the exact
  same instant.
- A literal IP address (no real DNS involved) is refreshed at the 1-hour
  ceiling, which is effectively a no-op since the address can never change.

If a refresh fails, WFX keeps using the last known-good addresses and retries
the resolution shortly after.

Each refresh resolves on a background thread and reports its result back to
the event loop through the engine's internal notification mechanism. A refresh
whose result is still waiting because the event loop hasn't drained it yet
counts against a small shared queue across all endpoints.

!!! danger
    If notifying the event loop about a completed refresh keeps failing,
    results pile up undrained and the shared queue can fill. Since a
    notification that can't reach the event loop means WFX has effectively
    lost the ability to ever tell it about new addresses, WFX treats a full
    queue as fatal for that worker rather than silently resolving DNS into a
    queue nothing is ever going to read. The worker exits and goes through the
    same restart path as any other worker crash, this is a last-resort guard
    against a broken event notification path, not something a healthy server
    will ever hit under normal DNS refresh traffic.

---

## TLS mode

`tlsConfig` decides whether a connection uses TLS:

```cpp
WFX::EpTlsAuto     // TLS on for well-known secure ports, off otherwise
WFX::EpTlsRequire  // Always use TLS, regardless of port
WFX::EpTlsInsecure // Always plaintext, even on a normally secure port
```

`EpTlsAuto` is the default and checks the port against a fixed list:

| Port | Protocol |
|------|----------|
| `443` | HTTPS |
| `465` | SMTPS |
| `563` | NNTPS |
| `636` | LDAPS |
| `989` | FTPS (data) |
| `990` | FTPS (implicit) |
| `992` | Telnet over TLS |
| `993` | IMAPS |
| `995` | POP3S |
| `5061` | SIPS |
| `5223` | XMPP over TLS |
| `5349` | STUNS / TURNS |
| `5671` | AMQPS |
| `5684` | CoAPS |
| `6380` | Redis TLS |
| `6514` | Syslog over TLS |
| `6697` | IRC over TLS |
| `8443` | HTTPS (alt) |
| `8883` | MQTTS |
| `9093` | Kafka TLS |

For any port outside that list, be explicit with `EpTlsRequire` or
`EpTlsInsecure` rather than relying on the heuristic.

---

## Coalescing

Picture 200 requests hitting your server at once, all asking for the same
popular resource on a slow upstream. Without coalescing, that's 200 separate
outbound connections (or 200 callers queued up waiting for a spot in a pool of
`connLimit` connections) all asking the upstream for the exact same thing.
Coalescing collapses that into **one** in-flight backend call: the first
request goes out for real, the other 199 are parked as waiters, and when the
real call finishes, every waiter gets its own copy of the result. From the
upstream's point of view, only one request ever happened.

Wiring it up takes two pieces on `EndpointDesc`:

1. **`coalesceKey`**, a function that turns a request into a `std::uint64_t` key. Two requests with the same key are treated as duplicates of each other. Returning `0` means "never coalesce this particular request" (useful for anything with side effects, like a `POST`).
2. **`cloneOutput`**, a function that deep-copies a `TRes`. This is **required** as soon as `coalesceKey` is set: every waiter needs its own independently-owned response object, not a shared pointer to the primary caller's.

```cpp
// Requests that hash to the same key are treated as duplicates of one another
std::uint64_t CoalesceByPath(const void* reqVoid) {
    auto& req = *static_cast<const ApiReq*>(reqVoid);
    return WFX::Shared::Hasher::Fnv1a(req.path.data(), req.path.size());
}

// Every waiter needs its own copy, ApiRes must be copyable
void* CloneApiRes(void* /*slotState*/, const void* srcOutput) {
    auto& src = *static_cast<const ApiRes*>(srcOutput);
    return WFX::New<ApiRes>(src);
}

inline const auto Api = WFX::Endpoint<ApiReq, ApiRes>{
    "api.example.com:80",
    WFX::EndpointDesc{
        .serialize     = Serialize,
        .parse         = Parse,
        .createOutput  = [](void*) -> void* { return WFX::New<ApiRes>(); },
        .destroyOutput = [](void* p) { WFX::Delete(static_cast<ApiRes*>(p)); },
        .coalesceKey   = CoalesceByPath,
        .cloneOutput   = CloneApiRes,
    },
    WFX::EndpointConfig{ .connLimit = 4, .requestTimeoutSeconds = 10 }
};
```

With this in place: the first caller asking for `/products/42` computes a key,
finds nothing in flight for it, and sends the request as normal. While that's
pending, more callers asking for the same path compute the same key, find a
match, and are parked as waiters instead of opening connections of their own.
Once the real response arrives, WFX calls `CloneApiRes` once per waiter (each
getting its own `EndpointOutput<ApiRes>`, freed independently through
`destroyOutput` when that caller's variable goes out of scope), then delivers
the original response to the caller who actually triggered the call.

Left `nullptr` (the default), coalescing never engages, every request opens or
reuses its own connection as usual.

!!! note
    Coalescing only applies to requests that arrive while an identical request
    is already in flight. It does not cache completed responses, the next
    request for the same key after the in-flight one finishes triggers a brand
    new backend call.

---

## `EndpointDesc`

`EndpointDesc` is where you describe your protocol to the engine: how to turn
a `TReq` into bytes, how to turn bytes back into a `TRes`, and a handful of
optional hooks for anything that needs to happen around a connection's
lifetime. Every field is a plain function pointer (or lambda that decays to
one), there's no interface to inherit from.

Two fields are required. Everything else is nullable, and only worth setting
if your protocol actually needs it, HTTP needs different hooks than Redis, so
most endpoints will leave most of this section untouched.

### The wire format

These two are the only required fields, an endpoint can't do anything without
them:

- **`serialize(slotState, &req, buf, bufLen, &written)`**: write the wire
  encoding of `req` into `buf`, and set `*written` to how many bytes you wrote.
  Return `WFX::EpSerOk` on success, `WFX::EpSerBufferTooSmall` if `buf` was too
  small (the engine grows the buffer and calls you again), or `WFX::EpSerError`
  on an unrecoverable failure.
- **`parse(slotState, parseState, buf, len, &consumed, outObj, isEof)`**: read
  bytes as they arrive off the wire and fill in `outObj` (a `TRes*`). Set
  `*consumed` to how many bytes you actually read, the engine keeps whatever's
  left for your next call. Return `WFX::EpParseDone` (or `WFX::EpParseClose` if
  the protocol wants the connection closed after this response) once `outObj`
  is complete, `WFX::EpParseIncomplete` if you need more bytes first, or
  `WFX::EpParseError` on a framing failure.

!!! danger
    When `isEof` is `true`, the peer has closed the connection and no more
    bytes are coming. You must not return `WFX::EpParseIncomplete` in that
    case, return `WFX::EpParseClose` or `WFX::EpParseError` instead. This is
    how [`HttpEndpoint`](http.md) supports HTTP/1.0-style bodies that end when
    the connection closes rather than by a length.

### Connection lifecycle

These run once per **physical connection**, not once per request. Use them
for anything that should survive across several keep-alive requests on the
same socket, a connection-level auth token, request counters, whatever your
protocol needs to remember between calls:

- **`createSlotState(userCtx)`** / **`destroySlotState(slotState)`**: allocate
  and free whatever state one connection needs to carry around. Called once
  when a slot first connects, and once when it's finally torn down, not on
  every request. `slotState` is the same pointer every other callback receives
  as its first argument.
- **`userCtx`**: an opaque pointer forwarded straight through to
  `createSlotState` as its argument, unchanged. Use it to hand the same
  connection factory a shared piece of config (an API key, a shared client
  object) without needing a global.
- **`onConnect`**: set automatically from the `OnConnect` template argument on
  `Endpoint<>`, see [onConnect handshakes](#onconnect-handshakes) below. Runs
  once per connection, right after the TCP/TLS handshake and before the slot
  is allowed to serve any request.
- **`onDisconnect(slotState, reason)`**: fires once, right before
  `destroySlotState`, whenever a slot is torn down for any reason. `slotState`
  is still valid when this runs, `reason` tells you why:

    | Reason | Meaning |
    |--------|---------|
    | `WFX::EpIdleTimeout` | The slot sat idle in the pool for `idleTimeoutSeconds` with nothing to do |
    | `WFX::EpHandshakeTimeoutReason` | The slot was closed while still connecting or handshaking, past `connectTimeoutSeconds` |
    | `WFX::EpDisconnectError` | Anything else: the peer closed the connection, a socket or protocol error, a forced close, exhausted reconnect attempts |

    Use this for connection-scoped cleanup or logging, closing a file handle
    `createSlotState` opened, decrementing a metric, whatever your protocol's
    `slotState` is holding onto that needs to be released or reported before
    it's freed.

### Per-request state and output

These run once per request, not once per connection:

- **`createParseState(slotState)`** / **`destroyParseState(parseState)`**:
  scratch space `parse()` needs while assembling one response, a partial
  header, a byte counter, anything that shouldn't leak into the next request
  on the same connection. Created before the first byte of a response is
  parsed and destroyed once `parse()` finishes.
- **`resetParseState(parseState)`**: if your parse state can be cheaply reset
  in place, provide this and the engine reuses the same `parseState` across
  keep-alive requests instead of paying a destroy-then-recreate every time.
  Optional, purely a performance knob.
- **`createOutput(slotState)`** / **`destroyOutput(outputPtr)`**: allocate and
  free the `TRes` your route handler ends up with. `createOutput` runs before
  `parse()` is ever called, `destroyOutput` runs when the caller's
  `EndpointOutput<T>` goes out of scope, freeing the response.

### Optional protocol features

Two pairs of callbacks unlock specific behavior. Both are documented in full,
with a worked example, in their own sections:

- **`coalesceKey(&req)`** / **`cloneOutput(slotState, srcOutput)`**: collapse
  duplicate in-flight requests into one backend call, see
  [Coalescing](#coalescing).
- **`hasCapacity(slotState)`** / **`takeStreamOutput(slotState, key)`**: run
  several requests concurrently over one connection, see
  [Multiplexing (advanced)](#multiplexing-advanced).

!!! important
    WFX validates all of this at startup, before the server accepts a single
    connection: a create/destroy pair must both be set or both left null, and
    `cloneOutput` must be set whenever `coalesceKey` is (same rule for
    `hasCapacity` and `takeStreamOutput`). A protocol that gets this wrong
    fails to start rather than crashing later on the first request that
    exercises the missing half.

### Putting it together

A small key-value protocol that uses most of the callbacks above: one
`AUTH <token>` line sent once per connection, then simple `GET <key>\n`
requests over the same socket, replies coming back as one line each.

```cpp
struct KvSlotState {
    std::uint32_t requestsServed = 0; // just to demonstrate connection-scoped state
};

struct KvParseState {
    WFX::String pending; // bytes seen so far for the response currently in progress
};

void* CreateSlotState(void* /*userCtx*/)
{
    return WFX::New<KvSlotState>();
}

void DestroySlotState(void* slotState)
{
    WFX::Delete(static_cast<KvSlotState*>(slotState));
}

void OnDisconnect(void* slotState, WFX::DisconnectReason reason)
{
    auto* state = static_cast<KvSlotState*>(slotState);
    if(reason == WFX::EpDisconnectError)
        WFX::LogWarn("kv connection dropped after ", state->requestsServed, " requests");
}

void* CreateParseState(void* /*slotState*/)
{
    return WFX::New<KvParseState>();
}

void DestroyParseState(void* parseState)
{
    WFX::Delete(static_cast<KvParseState*>(parseState));
}

void ResetParseState(void* parseState)
{
    static_cast<KvParseState*>(parseState)->pending.clear();
}

void* CreateOutput(void* /*slotState*/)
{
    return WFX::New<KvRes>();
}

void DestroyOutput(void* outputPtr)
{
    WFX::Delete(static_cast<KvRes*>(outputPtr));
}

SerializeResult Serialize(void* slotStateVoid, const void* reqVoid, char* buf, std::uint32_t bufLen,
                          std::uint32_t* written, std::uint64_t* /*streamKey, unused, no multiplexing*/)
{
    auto* state = static_cast<KvSlotState*>(slotStateVoid);
    auto& req = *static_cast<const KvReq*>(reqVoid);

    int n = std::snprintf(buf, bufLen, "GET %s\n", req.key.c_str());
    if(n < 0 || static_cast<std::uint32_t>(n) >= bufLen)
        return SerializeResult::BUFFER_TOO_SMALL;

    *written = static_cast<std::uint32_t>(n);
    state->requestsServed++;
    return SerializeResult::OK;
}

ParseResult Parse(void* /*slotState*/, void* parseStateVoid, const char* buf, std::uint32_t len,
                  std::uint32_t* consumed, void* outObjVoid, bool isEof, std::uint64_t* /*completedKey, unused*/)
{
    auto* parseState = static_cast<KvParseState*>(parseStateVoid);
    auto* out = static_cast<KvRes*>(outObjVoid);

    parseState->pending.append(buf, len);
    *consumed = len;

    auto pos = parseState->pending.find('\n');
    if(pos == WFX::String::npos)
        return isEof ? ParseResult::ERROR : ParseResult::INCOMPLETE;

    out->value = parseState->pending.substr(0, pos);
    return ParseResult::DONE;
}

inline const auto Kv = WFX::Endpoint<KvReq, KvRes, &Authenticate>{ // Authenticate: see onConnect handshakes
    "kv.internal:7000",
    WFX::EndpointDesc{
        .serialize         = Serialize,
        .parse             = Parse,
        .onDisconnect      = OnDisconnect,
        .createSlotState   = CreateSlotState,
        .destroySlotState  = DestroySlotState,
        .createParseState  = CreateParseState,
        .destroyParseState = DestroyParseState,
        .resetParseState   = ResetParseState,
        .createOutput      = CreateOutput,
        .destroyOutput     = DestroyOutput,
    },
    WFX::EndpointConfig{ .connLimit = 4, .requestTimeoutSeconds = 5 }
};
```

Walking through what actually happens the first time a route handler does
`co_await Kv.SendPayload({.key = "session:42"})`, assuming the pool is
completely cold:

1. No idle slot exists, so WFX opens a new TCP connection and calls
   `CreateSlotState`, its return value becomes `slotState` for every other
   callback on this connection from here on.
2. `Authenticate` (the `onConnect` coroutine) runs once, sends the `AUTH` line,
   waits for the reply. If it returns `WFX::EpReady`, the slot enters the pool.
3. `CreateOutput` allocates the `KvRes` your route handler will eventually get.
4. `CreateParseState` allocates a fresh `KvParseState`.
5. `Serialize` writes `GET session:42\n` into the send buffer.
6. As bytes come back, `Parse` is called repeatedly, appending to
   `parseState->pending`, until it finds a `\n` and fills `out->value`,
   returning `WFX::EpParseDone`.
7. Your route handler resumes, `out` is the populated `KvRes`.

If a second request comes in on the same keep-alive connection: `slotState`
is reused as-is (still remembers `requestsServed`), `ResetParseState` clears
`pending` instead of a fresh `CreateParseState`/`DestroyParseState` pair, and
`CreateOutput` allocates a new `KvRes` for this request. Eventually, if the
connection sits idle long enough, WFX closes it, `OnDisconnect` fires with
`WFX::EpIdleTimeout`, and `DestroySlotState` frees `slotState` for good.

---

## `onConnect` handshakes

Some protocols need a handshake before a connection is usable: TLS client
certs, a Redis `AUTH`, an SMTP `EHLO`. Pass a coroutine as the third template
argument to `Endpoint<>`:

```cpp
WFX::EpCoro Authenticate(WFX::SlotHandle h, void* /*state*/)
{
    static const char kAuth[] = "*2\r\n$4\r\nAUTH\r\n$8\r\npassword\r\n";
    if(co_await h.Send(kAuth, sizeof(kAuth) - 1) == WFX::EpSlotSendError)
        co_return WFX::EpFatal;

    auto recv = co_await h.Receive();
    if(recv.status == WFX::EpSlotRecvError)
        co_return WFX::EpFatal;

    co_return (recv.len >= 3 && recv.buf[0] == '+') ? WFX::EpReady : WFX::EpFatal;
}

inline const auto Redis = WFX::Endpoint<RedisGet, RedisVal, &Authenticate>{
    "redis.internal:6379",
    WFX::EndpointDesc{ .serialize = ..., .parse = ... },
    WFX::EndpointConfig{ .connLimit = 8, .requestTimeoutSeconds = 5 }
};
```

`onConnect` runs once per **physical connection**, not once per request, right
after the TCP (and TLS, if applicable) handshake finishes and before the slot
is allowed to serve any request. `WFX::SlotHandle` gives you `co_await h.Send(...)`
and `co_await h.Receive()` to drive the handshake yourself, plus
`h.NegotiatedProtocol()` to read back the ALPN protocol chosen during the TLS
handshake (empty if the connection isn't TLS, or the handshake hasn't finished).

Return one of:

- `WFX::EpReady`: handshake succeeded, the slot enters the pool
- `WFX::EpRetry`: transient failure, the engine reconnects with backoff (subject to the same fail-fast-if-a-caller-is-waiting rule described [above](#reconnects-and-backoff))
- `WFX::EpFatal`: permanent failure, the slot is discarded

The whole budget for connect + TLS + `onConnect` combined is
`connectTimeoutSeconds`. If it isn't done in time, the attempt is treated as a
connect failure.

---

## Multiplexing (advanced)

By default, one slot serves exactly one request at a time; the next request
waits for a free slot or opens a new connection. Some protocols (HTTP/2-style
framing over a single TCP connection, for example) can multiplex several
concurrent requests onto one connection instead, tagging each request and
response with an ID so replies can come back in any order and still be
matched to the right caller.

To support that, `serialize()` assigns every outgoing request a `streamKey`
(instead of leaving it at `0`, which means "this protocol doesn't multiplex"),
and three `EndpointDesc` callbacks work together to track requests sharing one
connection:

- **`hasCapacity(slotState)`**: does this slot have room to take on one more request right now?
- **`takeStreamOutput(slotState, key)`**: hand over a finished stream's response, or `nullptr` if it isn't done yet.
- **`EndpointConfig::maxConcurrentStreams`**: how many requests are allowed to share one slot (`0` or `1` keeps a slot exclusive, the default).

Unlike the single-request-per-slot path, the engine never *calls*
`createOutput` for a multiplexed request. The protocol owns each in-flight
response internally (however it wants to store it, keyed by `streamKey`) and
only hands ownership to the engine once `takeStreamOutput` returns it.
`destroyOutput` is still used, to free a response once the caller's
`EndpointOutput<T>` goes out of scope, or to clean up a response that finished
but was abandoned (its caller disconnected first).

!!! important
    `createOutput` still has to be **set**, even though it's never called on
    this path. Startup validation checks `createOutput`/`destroyOutput` as a
    pair regardless of whether `hasCapacity` is set, so a multiplexed protocol
    that only sets `destroyOutput` fails to start. A stub that just returns
    `nullptr` is all it needs.

A sketch of a simple request/response protocol that multiplexes by tagging
every frame with a 32-bit request ID:

```cpp
struct PendingReply {
    std::uint32_t id;
    MyRes* res; // owned until takeStreamOutput hands it off
};

struct MySlotState {
    std::uint32_t nextId = 0;
    std::vector<PendingReply> finished; // replies fully parsed, waiting to be claimed
    std::uint32_t inFlight = 0;         // requests sent but not yet finished
};

SerializeResult Serialize(void* slotStateVoid, const void* req, char* buf, std::uint32_t bufLen,
                          std::uint32_t* written, std::uint64_t* streamKey)
{
    auto* state = static_cast<MySlotState*>(slotStateVoid);
    std::uint32_t id = state->nextId++;

    // ... write a frame containing 'id' and the serialized request into buf ...

    *streamKey = id;   // tags this request so the matching reply can be found later
    state->inFlight++;
    return SerializeResult::OK;
}

ParseResult Parse(void* slotStateVoid, void* parseState, const char* buf, std::uint32_t len,
                  std::uint32_t* consumed, void* /*outObj, unused here*/, bool isEof,
                  std::uint64_t* completedKey)
{
    auto* state = static_cast<MySlotState*>(slotStateVoid);

    // ... read one complete frame off buf, extract its request id and payload ...
    std::uint32_t id = /* parsed from the frame */ 0;
    auto* res = WFX::New<MyRes>(/* ... fill from the frame's payload ... */);

    state->finished.push_back({id, res});
    state->inFlight--;
    *completedKey = id; // tells the engine this particular stream just finished

    // The connection itself stays open for other in-flight streams, so this-
    // -frame being done does not mean there is nothing left to read
    return ParseResult::INCOMPLETE;
}

bool HasCapacity(void* slotStateVoid)
{
    auto* state = static_cast<MySlotState*>(slotStateVoid);
    return state->inFlight < 32; // matches EndpointConfig::maxConcurrentStreams below
}

void* TakeStreamOutput(void* slotStateVoid, std::uint64_t key)
{
    auto* state = static_cast<MySlotState*>(slotStateVoid);

    for(auto it = state->finished.begin(); it != state->finished.end(); ++it) {
        if(it->id == static_cast<std::uint32_t>(key)) {
            void* res = it->res;
            state->finished.erase(it);
            return res; // ownership transfers to the caller
        }
    }

    return nullptr; // not finished yet
}

inline const auto Api = WFX::Endpoint<MyReq, MyRes>{
    "api.example.com:443",
    WFX::EndpointDesc{
        .serialize        = Serialize,
        .parse            = Parse,
        .createOutput     = [](void*) -> void* { return nullptr; }, // never called here, just satisfies the pairing check
        .destroyOutput    = [](void* p) { WFX::Delete(static_cast<MyRes*>(p)); },
        .hasCapacity      = HasCapacity,
        .takeStreamOutput = TakeStreamOutput,
    },
    WFX::EndpointConfig{ .connLimit = 4, .maxConcurrentStreams = 32 }
};
```

This is only useful if you're implementing a protocol that actually supports
request multiplexing. [`HttpEndpoint`](http.md) does not use it, since HTTP/1.1
has no concept of concurrent requests on one connection.

---

## Writing protocol-agnostic code

`Endpoint<>`, `EndpointOutput<T>`, `SlotHandle`, and the rest of this primitive
have nothing HTTP-specific about them, they live in `WFX::Async` under the hood
and are re-exported through `WFX::` so a protocol client for Redis, Postgres,
or anything else reads the same way an HTTP one does. See [`HttpEndpoint`](http.md)
for a complete, worked example of a protocol built on top of this primitive.
