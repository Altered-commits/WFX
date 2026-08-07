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
(`SmtpTransaction`) instead of a single request/response pair.

You have two ways to drive that exchange:

- **[`SendMail`](#sendmail)**: one call that runs the whole thing for you.
  Right for almost every case: a contact form, a signup confirmation, any
  place you just want to know whether the email went out.
- **[`SmtpTransaction`](#smtptransaction)**, step by step: for when you need
  to react differently depending on which command failed (for example,
  showing "that recipient doesn't exist" instead of a generic error), or when
  you want to send to several recipients on one connection by calling
  `RcptTo` more than once before `DataStart`.

---

## `SendMail`

The one-call version. It opens a transaction, runs `MAIL FROM`, `RCPT TO`,
`DATA`, and the message body through it, and stops at the first thing that
goes wrong.

```cpp
WFX_POST("/contact", [](WFX::Request req, WFX::Response res) -> WFX::Coro {
    WFX::SmtpSendOutcome outcome;

    co_await Relay.SendMail(
        "noreply@example.com",  // fromAddr:  envelope + From: header
        "Example",               // fromName:  display name on From:
        "contact@example.com",   // toAddr:    envelope + To: header
        "Contact Form",          // toName:    display name on To:
        "New enquiry",           // subject
        "message body",          // body
        outcome
    );

    if(!outcome.Success()) {
        res.Status(WFX::HttpStatus::BAD_GATEWAY).SendText("mail failed");
        co_return;
    }

    res.SendText("sent");
    co_return;
});
```

**`SmtpSendOutcome`**:

```cpp
struct SmtpSendOutcome {
    Shared::EndpointStatus status; // EpOk once DATA actually goes through
    EndpointOutput<SmtpResponse> response; // the last reply that came back, if any

    bool Success() const noexcept; // status == EpOk && response && response->Success()
};
```

- **`status`** tells you whether a real SMTP conversation even happened. It's
  `WFX::EpOk` once the message is fully sent. Anything else means the
  problem was below the SMTP layer entirely: the connection pool was full
  (`WFX::EpPoolExhausted`), the relay refused the TCP/TLS connection, or a
  command timed out. A real reply from the server was never received in any
  of those cases.
- **`response`** stays empty (`bool(outcome.response) == false`) exactly when
  that happens. Once a real SMTP reply exists, `response` holds it: `code`
  and `text`, the same shape `SmtpTransaction`'s own methods return, see
  [`SmtpResponse`](#smtpresponse) below.
- **`Success()`** is the one thing most callers need: true only if the whole
  four-step exchange completed and the server accepted it.

If you need to know exactly which step failed (bad sender vs. bad recipient
vs. the message itself getting rejected), `SendMail` won't tell you that on
its own. Drop down to [`SmtpTransaction`](#smtptransaction) and run the
steps yourself, checking each response as it comes back.

---

## `SmtpTransaction`

SMTP isn't one request/response, it's a small conversation, and the server
keeps track of that conversation's state (who the mail is from, who it's
going to, whether it's currently in the middle of receiving a message body)
tied to the one TCP connection it's happening on. That's the whole reason
`SmtpTransaction` exists: it's a handle to one pinned connection, and every
method on it sends the next command in that same conversation.

```cpp
auto tx = Relay.Begin();
if(!tx.IsValid()) { /* pool exhausted, every pooled connection is busy */ }

tx.MailFrom(addr);
tx.RcptTo(addr);
tx.DataStart();
tx.DataBody(fromAddr, fromName, toAddr, toName, subject, body, replyTo = {});
tx.Reset();
tx.Quit();
```

Every method returns the same `{status, out}` pair described in
[Sending a request](overview.md#sending-a-request), where `out` is a
`WFX::EndpointOutput<SmtpResponse>`. Check `status` first (it covers
transport-level failures like a timeout, where `out` never gets filled in at
all), then check the response itself, see [`SmtpResponse`](#smtpresponse)
below for what `Success()` and `Continue()` mean.

### `Begin()` and `IsValid()`

`Begin()` reserves one connection from the pool for you, exactly like
[`Reserve()`](overview.md#pinning-a-connection) on the raw primitive. You get
it back wrapped in an `SmtpTransaction`. Every command you send through that
`tx` runs on that same connection, which is what lets the server follow along
with one mail transaction instead of seeing unrelated commands from different
connections.

Always check `IsValid()` before calling anything else on `tx`. It comes back
false when every connection in the pool is already busy (bounded by
`connLimit`, see [Tuning & limits](#tuning-limits) below), there simply
wasn't a free connection to hand you.

### `MailFrom(addr)`

Sends `MAIL FROM:<addr>`, the first command of any transaction. This is the
*envelope* sender, the address the receiving server uses for bounces and
delivery decisions. It's a separate thing from the human-readable `From:`
header you'll set later in `DataBody`, they're usually the same address but
don't have to be. A success reply is `250`.

### `RcptTo(addr)`

Sends `RCPT TO:<addr>`, the envelope recipient. Real SMTP lets you call this
more than once before `DataStart` to send the same message to several
recipients on one connection, `SmtpTransaction` doesn't stop you from doing
that either. A success reply is `250`; a common rejection is `550` (no such
user, or the relay won't deliver to that address).

### `DataStart()`

Sends `DATA`, announcing that the message content is about to follow. The
server's reply here is not a normal `2xx`, it's `354` ("start mail input, end
with a line containing just a dot"), which is exactly why `SmtpResponse` has
a separate `Continue()` check instead of reusing `Success()` for this one
step.

### `DataBody(fromAddr, fromName, toAddr, toName, subject, body, replyTo = {})`

Sends the actual message: headers, then a blank line, then the body, then the
terminator. It builds all of this for you from the arguments:

- **`fromAddr` / `fromName`** become the `From:` header, e.g.
  `From: Example <noreply@example.com>`. `fromName` can be empty, in which
  case `From:` is just the bare address.
- **`toAddr` / `toName`** become `To:` the same way.
- **`subject`** becomes the `Subject:` header, unmodified.
- **`body`** is the message text. It gets dot-stuffed automatically (RFC 5321
  4.5.2: a body line starting with `.` is escaped to `..`, so a line in your
  message that happens to start with a period can never be mistaken for the
  terminator that ends the `DATA` block).
- **`replyTo`** is optional. If you pass one, a `Reply-To:` header is added.

`Date`, `Message-ID`, and `MIME-Version`/`Content-Type` headers are added on
top of that automatically, you don't provide them. Every one of the
caller-supplied fields above is screened for CR/LF/NUL before any of this
reaches the wire, see [Injection defenses](#injection-defenses) below. A
success reply is `250`.

### `Reset()`

Sends `RSET`, which clears whatever `MAIL FROM`/`RCPT TO`/`DATA` state has
built up so far, without closing the connection. Use it when you want to
start a second, unrelated transaction on the same already-authenticated
connection instead of paying for a new connection and a new handshake, for
example after a recipient gets rejected and you want to try a different one
from scratch.

### `Quit()`

Sends `QUIT`, telling the server you are completely finished with this
connection. The server typically closes its end after replying `221`.

!!! warning "Quit() is not cleanup, it ends the connection"
    You do not need to call `Quit()` when you're done with a `tx`. Letting it
    go out of scope already returns the connection to the pool for reuse
    (`ReservedSlot`'s destructor does this, see
    [Pinning a connection](overview.md#pinning-a-connection)), and that
    happens purely on WFX's side, no bytes get sent to the server for it.
    `Quit()` is the opposite: it actively tells the *server* to close the TCP
    connection. Only call it if you deliberately want this specific
    connection gone instead of sitting in the pool for the next `Begin()` to
    reuse.

---

## Coalescing

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
