# SMTP Endpoint

`WFX::SmtpEndpoint` is a ready-to-use SMTP submission client, built on top of
the raw [`WFX::Endpoint<>` primitive](overview.md). One instance represents
one upstream relay and its connection pool.

!!! important
    This page only covers what's specific to SMTP. The connection pool, DNS
    resolution and refresh, reconnect/backoff behavior, and prewarm all work
    exactly as described on the [Endpoint overview](overview.md),
    `SmtpEndpoint` just wires them up for you with an SMTP-shaped handshake
    and transaction.

    To use it, include:
    ```cpp
    #include <wfx/endpoint/smtp.hpp>
    ```

Speaks STARTTLS submission on port 587 (RFC 3207): `EHLO` -> `STARTTLS` ->
`EHLO` again over the encrypted channel -> `AUTH PLAIN` or `AUTH LOGIN`,
whichever the server advertises post-TLS. Implicit TLS (port 465) is out of
scope. CRAM-MD5 and other legacy AUTH mechanisms are deliberately not
implemented, they exist only to protect credentials on an unencrypted line,
and this client never sends credentials on one.

---

## Declaring & sending

```cpp
inline const auto Relay = WFX::SmtpEndpoint{"smtp.example.com:587", WFX::SmtpEndpointConfig{
    .username = WFX::GetEnvString("SMTP_USERNAME"),
    .password = WFX::GetEnvString("SMTP_PASSWORD"),
    .heloName = "mail.example.com",
}};

WFX_POST("/contact", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    auto tx = Relay.Begin();
    if(!tx.IsValid()) {
        res.Status(WFX::HttpStatus::SERVICE_UNAVAILABLE).SendText("busy");
        co_return;
    }

    auto [s1, r1] = co_await tx.MailFrom("noreply@example.com");
    if(s1 != WFX::EpOk || !r1->Success()) {
        res.Status(WFX::HttpStatus::BAD_GATEWAY).SendText("mail failed");
        co_return;
    }

    auto [s2, r2] = co_await tx.RcptTo("contact@example.com");
    if(s2 != WFX::EpOk || !r2->Success()) {
        res.Status(WFX::HttpStatus::BAD_GATEWAY).SendText("mail failed");
        co_return;
    }

    auto [s3, r3] = co_await tx.DataStart();
    if(s3 != WFX::EpOk || !r3->Continue()) {
        res.Status(WFX::HttpStatus::BAD_GATEWAY).SendText("mail failed");
        co_return;
    }

    auto [s4, r4] = co_await tx.DataBody("noreply@example.com", "Example", "contact@example.com",
                                         "Contact Form", "New enquiry", "message body");
    if(s4 != WFX::EpOk || !r4->Success()) {
        res.Status(WFX::HttpStatus::BAD_GATEWAY).SendText("mail failed");
        co_return;
    }

    res.SendText("sent");
    co_return;
});
```

Declaration rules (namespace scope, `"host:port"` format, no scheme prefix,
eager DNS resolution at startup) are the same as the raw primitive, see
[Declaring an endpoint](overview.md#declaring-an-endpoint) for the full
details.

Unlike `HttpEndpoint`, one call doesn't stand alone: a mail transaction is
several commands (`MAIL FROM`, `RCPT TO`, `DATA`) that must run on the same
connection, so `SmtpEndpoint` hands you a [pinned connection](overview.md#pinning-a-connection)
(`SmtpTransaction`) instead of a single request/response pair. Each step is
awaited explicitly, there is no one call that does the whole transaction for
you, see [Writing protocol-agnostic code](overview.md#writing-protocol-agnostic-code)
for why: this codebase's coroutine machinery is a closed set of specializations
with nowhere for a "await several sub-operations, hand back one result" helper
to live outside your own route handler.

---

## `SmtpTransaction`

```cpp
auto tx = Relay.Begin();
if(!tx.IsValid()) { /* pool exhausted */ }

tx.MailFrom(addr);
tx.RcptTo(addr);
tx.DataStart();
tx.DataBody(fromAddr, fromName, toAddr, toName, subject, body, replyTo = {});
tx.Reset();
tx.Quit();
```

Every method returns the same `{status, out}` pair described in
[Sending a request](overview.md#sending-a-request), where `out` is a
`WFX::EndpointOutput<SmtpResponse>`. `Begin()` pins one connection for the
whole exchange exactly as [`Reserve()`](overview.md#pinning-a-connection)
does, always check `IsValid()` before use, the pool can be exhausted.

`DataBody` builds the `From`/`To`/`Reply-To`/`Subject` headers and a
dot-stuffed body (RFC 5321 4.5.2: a body line starting with `.` is escaped to
`..` so it can't be mistaken for the `DATA` terminator) internally, along with
a `Date`, `Message-ID`, and `MIME-Version`/`Content-Type` header. Every
caller-supplied field is CR/LF/NUL-screened before it reaches the wire, see
[Injection defenses](#injection-defenses) below.

`SmtpEndpoint` does not wire up [coalescing](overview.md#coalescing):
`coalesceKey`/`cloneOutput` are left null in its `EndpointDesc`. `MAIL FROM`
/ `RCPT TO` / `DATA` are side-effecting, sending an email, so deduping two
concurrent calls into one backend transaction would silently drop a real
message rather than save a redundant one. It also wouldn't apply in practice
even if wired up: every send goes through `Begin()`'s pinned connection, and
[pinned sends are exempt from coalescing](overview.md#pinning-a-connection)
by the primitive's own rule.

---

## `SmtpResponse`

```cpp
struct SmtpResponse {
    std::uint16_t code = 0;
    WFX::String text;

    bool Success() const noexcept; // code >= 200 && code < 300
    bool Continue() const noexcept; // code == 354, DATA's go-ahead
};
```

`code` and `text` are the *final* line of a (possibly multi-line) SMTP
response. `Success()` checks the 2xx range; `Continue()` checks specifically
for `354`, the go-ahead `DataStart()` expects before you send `DataBody()`.

---

## Tuning & limits

```cpp
inline const auto Relay = WFX::SmtpEndpoint{"smtp.example.com:587", WFX::SmtpEndpointConfig{
    .connLimit             = 4,
    .connectTimeoutSeconds = 20,
    .requestTimeoutSeconds = 20,
    .username              = WFX::GetEnvString("SMTP_USERNAME"),
    .password              = WFX::GetEnvString("SMTP_PASSWORD"),
    .heloName              = "mail.example.com",
}};
```

| Setting | Meaning |
|---------|---------|
| `connLimit` | Max simultaneous connections to this relay, per worker (default 4) |
| `connectTimeoutSeconds` | Budget for TCP connect + `EHLO`/`STARTTLS`/`EHLO`/`AUTH` combined (default 20) |
| `requestTimeoutSeconds` | Budget for one `MAIL`/`RCPT`/`DATA` command round trip (default 20) |
| `idleTimeoutSeconds` | Idle pooled connections close after this long (default 60) |
| `maxReconnectAttempts` / `reconnectBackoffBaseSeconds` / `reconnectBackoffMaxSeconds` | Same reconnect/backoff behavior as [the primitive](overview.md#reconnects-and-backoff) |
| `prewarm` | Connections to open eagerly on startup |
| `maxResponseLineBytes` | Cap on one response line's size (default 2048), a DoS defense against an endless line |
| `maxResponseLines` | Cap on the number of lines in one multi-line response (default 64), a DoS defense against an endless `250-...` flood |
| `username` / `password` | Credentials offered via `AUTH PLAIN` or `AUTH LOGIN`, whichever the server advertises. Never logged |
| `heloName` | The identity sent in `EHLO`, e.g. `"mail.example.com"` |

There is no `tlsConfig` on `SmtpEndpointConfig`: STARTTLS is negotiated
in-band by the client's own `onConnect` handshake, the underlying connection
is always plaintext until that handshake upgrades it, see
[In-band TLS upgrades](overview.md#in-band-tls-upgrades).

---

## Security posture

Every one of these is load-bearing, not incidental:

- **`AUTH` is refused outright if the server never advertised `STARTTLS`.**
  There is no silent plaintext-credential fallback in this client, ever, see
  [`no_starttls`/`inject` audit coverage](../../dev_reference/testing.md).
- **The pre-TLS `EHLO` capability list is discarded and re-fetched after the
  TLS upgrade completes.** Trusting the pre-TLS list would let a MITM strip
  the `STARTTLS`/`AUTH` capabilities off the wire and force a downgrade (the
  CVE-2011-0411-class STARTTLS-stripping attack).
- **Whatever's buffered before the TLS wrap is discarded before the
  handshake starts.** A malicious relay can append plaintext right after its
  `"220 Ready to start TLS"` go-ahead, before the real handshake even begins.
  A byte sequence like that was never a valid TLS record to start with, so
  the handshake either never sees it (it was already discarded) or fails
  outright trying to parse it as one, either way it is never read back as
  though the authenticated peer had sent it, and never reaches an
  authenticated `MAIL`/`RCPT`/`DATA` exchange.
- **Every user-suppliable field** (envelope addresses, display names,
  subject, `Reply-To`) is scanned for CR/LF/NUL before it reaches the wire,
  see [Injection defenses](#injection-defenses).
- **The message body is dot-stuffed** so a line starting with `.` can't be
  mistaken for the `DATA` terminator.
- **Server certificate verification reuses the engine's existing outbound TLS
  trust decision.** No SMTP-specific certificate logic exists to get wrong.
- **Multi-line responses are bounded** (`maxResponseLineBytes` /
  `maxResponseLines`) so a hostile or compromised relay can't OOM or hang a
  worker with an endless response.

### Injection defenses

`MailFrom`, `RcptTo`, and every `DataBody` field are scanned for CR, LF, and
NUL before serialization; a hit fails the call outright (`WFX::EpSerializeError`,
surfaced as the request's status) rather than emitting a partially-escaped
command. This is SMTP's analogue of HTTP header/request-line injection: an
address or subject containing `"\r\nMAIL FROM:<attacker>"` is exactly the
kind of smuggled-command attack this closes off. `heloName` gets the same
screening at connect time, before the first byte of any handshake is sent.
