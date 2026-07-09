# HTTP Endpoint

`WFX::HttpEndpoint` is a ready-to-use HTTP/1.1 client, built on top of the raw
[`WFX::Endpoint<>` primitive](overview.md). One instance represents one upstream
host and its connection pool.

!!! important
    This page only covers what's specific to HTTP. The connection pool, DNS
    resolution and refresh, TLS mode, reconnect/backoff behavior, prewarm, and
    coalescing all work exactly as described on the [Endpoint overview](overview.md),
    `HttpEndpoint` just wires them up for you with an HTTP-shaped request/response.

    To use it, include:
    ```cpp
    #include <wfx/endpoint/http.hpp>
    ```

---

## Declaring & sending

```cpp
inline const auto Api = WFX::HttpEndpoint{"api.example.com:443"};

WFX_GET("/proxy", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    auto [status, out] = co_await Api.Get("/users/42");
    if(status != WFX::EpOk) {
        res.Status(502).SendText("upstream error");
        co_return;
    }

    res.Status(out->status).SendText(out->body);
    co_return;
});
```

Declaration rules (namespace scope, `"host:port"` format, no scheme prefix,
eager DNS resolution at startup) and the `status`/`out` pair returned by every
call are the same as the raw primitive, see
[Declaring an endpoint](overview.md#declaring-an-endpoint) and
[Sending a request](overview.md#sending-a-request) for the full details and
the complete status code table.

It supports keep-alive, chunked transfer encoding, `Content-Length` and
close-delimited bodies, `HEAD`/`204`/`304`/`1xx` no-body responses, and
HTTP/1.0's default-close behavior. It does **not** support `CONNECT`
tunneling, protocol upgrades, trailer headers, or `Transfer-Encoding` codings
other than plain `chunked`.

---

## Making requests

One method per HTTP verb, all returning the same awaitable pair described in
[Sending a request](overview.md#sending-a-request):

```cpp
Api.Get(path, headers = {});
Api.Head(path, headers = {});
Api.Options(path, headers = {});
Api.Delete(path, headers = {});
Api.Post(path, body, headers = {});
Api.Put(path, body, headers = {});
Api.Patch(path, body, headers = {});
```

Or build the request yourself with `Send`:

```cpp
WFX::HttpEndpointRequestHeaders hdrs;
hdrs.Add("Content-Type", "application/json");
hdrs.Add("Authorization", authHeaderView); // must outlive the co_await

auto [status, out] = co_await Api.Post("/users", jsonBody, hdrs);
```

`Send` takes an `HttpEndpointRequest` directly:

```cpp
struct HttpEndpointRequest {
    HttpMethod method = HttpMethod::GET;
    std::string_view path = "/";
    std::string_view body{};
    HttpEndpointRequestHeaders headers{};
};
```

!!! important
    `path`, `body`, and header values passed into these calls are caller-owned
    `std::string_view`s. They must stay alive for the entire duration of the
    `co_await`, WFX does not copy them for you.

`Host`, `Content-Length`, and `Transfer-Encoding` are always set by WFX itself;
any of those you add yourself in `headers` are silently dropped in favor of
the engine's own values. `HttpEndpointRequestHeaders` is case-insensitive on
lookup (`Get`, `Contains`) and `Set` (replaces an existing header instead of
duplicating it); `Add` always appends.

---

## `HttpEndpointResponse`

```cpp
struct HttpEndpointResponse {
    std::uint16_t status;
    WFX::String body;
    HttpEndpointResponseHeaders headers;

    bool GetHeader(std::string_view name, std::string_view& out) const noexcept;
    bool IsSuccess() const noexcept; // 200 <= status < 300
};
```

This is what `out` points to after a successful `co_await`. Header lookups are
case-insensitive, same as on the request side.

---

## Tuning & limits

```cpp
inline const auto Api = WFX::HttpEndpoint{"api.example.com:443", WFX::HttpEndpointConfig{
    .connLimit             = 16,
    .requestTimeoutSeconds = 5,
    .maxBodyBytes          = 4 * 1024 * 1024,
}};
```

`HttpEndpointConfig` carries every pool setting described in
[Pool settings](overview.md#pool-settings) (`connLimit`, `connectTimeoutSeconds`,
`requestTimeoutSeconds`, `idleTimeoutSeconds`, `maxReconnectAttempts`,
`reconnectBackoffBaseSeconds`, `reconnectBackoffMaxSeconds`, `tlsConfig`,
`prewarm`), plus HTTP-specific protocol hardening knobs:

| Setting | Meaning |
|---------|---------|
| `maxHeaderBytes` | Cap on the status line + header block size (default 16 KiB) |
| `maxBodyBytes` | Cap on the response body size (default 16 MiB) |
| `maxHeaderCount` | Cap on the number of response headers, an amplification defense (default 100) |
| `coalesceKey` | Dedup function, see [Coalescing](#coalescing) below |

There is no `dnsRefreshSeconds` on `HttpEndpointConfig`; `HttpEndpoint` always
follows the DNS record's own TTL.

Request paths and headers containing CR, LF, or NUL bytes are rejected outright
(the request fails to serialize with `WFX::EpSerializeError`) rather than being
sent to the upstream, this closes off response-splitting-style attacks through
values your route handler forwards into an outbound request. On the response
side, conflicting or ambiguous `Content-Length` / `Transfer-Encoding`
combinations are rejected as malformed rather than guessed at, and obsolete
header line folding (`obs-fold`) is rejected rather than parsed.

---

## Coalescing

Works exactly as described in [Coalescing](overview.md#coalescing), except the
key function receives an `HttpEndpointRequest` (cast it yourself, same as any
other raw `EndpointDesc` callback):

```cpp
std::uint64_t MyCoalesce(const void* reqVoid) {
    auto& req = *static_cast<const WFX::HttpEndpointRequest*>(reqVoid);
    if(req.method != WFX::HttpMethod::GET)
        return 0; // never coalesce non-GET requests

    return WFX::Shared::Hasher::Fnv1a(req.path.data(), req.path.size());
}

inline const auto Api = WFX::HttpEndpoint{"api.example.com:443",
    WFX::HttpEndpointConfig{.coalesceKey = &MyCoalesce}};
```

Left `nullptr` (the default), coalescing never engages.
