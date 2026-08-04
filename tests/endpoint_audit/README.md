# WFX Endpoint Audit

Adversarial correctness and security testing for **outbound** connections:
`WFX::HttpEndpoint`, `WFX::SmtpEndpoint`, and the raw `WFX::Endpoint<>`
primitive they're both built on (`include/wfx/endpoint/base.hpp`,
`include/wfx/endpoint/http.hpp`, `include/wfx/endpoint/smtp.hpp`).

Where `tests/base_audit` tortures WFX as an inbound **server**, this suite
tortures it as a **client**, in four parts:

- **HTTP/1.1** (`WFX::HttpEndpoint`): stands up a fully hostile upstream and
  fires 300+ attack vectors *through* WFX at it, asserting the client-side
  parser/serializer never crashes, never hangs past its timeout, never
  mis-frames one response into the next, never smuggles a request, never leaks
  one request's data into another, and never lets a malicious upstream poison a
  pooled keep-alive connection.
- **SMTP** (`WFX::SmtpEndpoint`): drives full `EHLO`/`STARTTLS`/`EHLO`/`AUTH`/
  `MAIL`/`RCPT`/`DATA` transactions against a second, dedicated hostile mock
  (one listener per persona), covering the handshake state machine, both AUTH
  mechanisms, STARTTLS injection/downgrade attacks, cert rejection, connect-
  and mid-transaction hangs, abrupt drops at every handshake stage, and
  CR/LF/NUL injection through every caller-supplied field.
- **Raw protocol** (`WFX::Endpoint<>` directly): HTTP/1.1 has no connection
  handshake and no concept of concurrent requests sharing one connection, so a
  second, tiny hand-rolled protocol drives `onConnect`, `onDisconnect`, and
  multiplexing directly: the three things `HttpEndpoint` structurally cannot
  reach.
- **SP protocol** (`WFX::Endpoint<>`, *non*-multiplexed): the raw protocol above
  sets `hasCapacity`, which puts it permanently on the multiplexed path. Slot
  pinning and streaming are single-slot-only by construction, and `onPush` only
  fires on a slot with nothing in flight, so a third protocol on its own listener
  covers **`Reserve`/`Release`**, **`Stream`** (both chunk families), **`onPush`**,
  and the cert-free half of **`UpgradeToTLS`**.

---

## Scope: this suite vs `tests/tls_audit`

Both suites touch TLS, so the boundary is worth stating plainly.

**`tls_audit` owns the certificate and trust surface.** Endpoints there are TLS
from the first byte (`EpTlsRequire`), and it asserts what TLS itself must do:
untrusted / hostname-mismatched / expired certs are refused, version downgrade is
refused, sessions resume. It needs `mkcert` and a CA-trust step.

**This suite owns everything else about the outbound client**, and stays
plaintext so it needs no certificates.

`UpgradeToTLS` is split across both, because its vectors have different needs:

| Vector | Suite | Why |
|---|---|---|
| Server refuses to upgrade, protocol requires TLS → must fail closed | **endpoint** | No handshake needed; the connection never becomes TLS |
| Server agrees, then sends garbage instead of a ServerHello | **endpoint** | The handshake is *meant* to fail; no valid cert required |
| Pre-upgrade plaintext must be discarded at the boundary | **tls** | Needs a *working* handshake to establish the trust boundary the injected bytes cross |
| `UpgradeToTLS` on an already-secure slot → refused | **tls** | Needs a slot the engine already wrapped, i.e. a real TLS endpoint |

The rule of thumb: if a vector needs a handshake to *succeed*, it lives in
`tls_audit`; if it only needs one to be *attempted*, it lives here.

`WFX::SmtpEndpoint`'s STARTTLS surface doesn't fit that split: it's an
in-band upgrade of an already-open plaintext connection, never TLS from the
first byte, so it never matches `tls_audit`'s `EpTlsRequire` model in the
first place. Its whole TLS surface, including certificate rejection
(`smtp_certs`), lives entirely in this suite instead, using `smtp_upstream.py`'s
own cert generation rather than `tls_audit`'s.

The suite deliberately thinks like an attacker: beyond the legal-matrix and
malformed-input corpora, it drives request smuggling, CRLF injection,
keep-alive **poisoning** (via oversized bodies, chunked framing, and no-body
statuses), trailer and 1xx **header-leak** probes, cross-request **bleed** on
a reused connection, coalescing cross-delivery (info disclosure), DoS caps
(header-byte and header-count amplification), an auth-handshake bypass check
(`onConnect`), and cross-request bleed under multiplexing (one caller
receiving another caller's response). Every byte the client puts on the wire
can also be inspected verbatim to prove no line break was smuggled into a
serialized request.

Everything is coordinated by the `.py` runner, exactly like `tests/base_audit`;
you never launch the WFX server yourself.

---

## How to run

```bash
cd tests/endpoint_audit

# Everything
python3 endpoint_audit.py

# One phase
python3 endpoint_audit.py --phase statusline
python3 endpoint_audit.py --phase desync

# List phases
python3 endpoint_audit.py --list-phases

# CI mode: no colors, GitHub Actions log groups and error annotations
python3 endpoint_audit.py --ci
```

The runner:

1. launches `http_upstream.py` (the hostile mock) on `127.0.0.1:8091`, plus its
   second raw-protocol listener on `127.0.0.1:8092`,
2. `wfx run app --detach` (builds and boots the client app),
3. drives every vector through WFX and collects findings,
4. stops the app (`wfx control stop app`) and the mock,
5. prints a per-phase report and exits.

### Requirements

- `wfx` on `PATH` (or `--wfx /path/to/wfx`)
- Python 3.8+, standard library only
- Linux

### Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--host` | `127.0.0.1` | Host for both WFX and the mock |
| `--port` | `8080` | WFX inbound port |
| `--up-port` | `8091` | Mock upstream port, must match `UPSTREAM` in `app/src/main.cpp` |
| `--proto-port` | `8092` | Mock proto-upstream port, must match `PROTO_UPSTREAM` in `app/src/main.cpp` |
| `--wfx` | `wfx` | Path to the wfx binary |
| `--app-dir` | `app` | App directory passed to `wfx run` |
| `--ready-timeout` | `30` | Seconds to wait for `/health` |
| `--phase` | `all` | Run a single phase |
| `--list-phases` | n/a | Print phases and exit |
| `--wfx-logs` | `important` | Stream WFX worker/master logs live: `off`, `important` (WRN/ERR/FTL + crash keywords), or `all` |
| `--ci` | off | No colors; emit GitHub Actions `::group::`/`::error::` commands |

> Both mock ports are **compile-time baked** into the WFX app (`#define UPSTREAM
> "127.0.0.1:8091"` / `#define PROTO_UPSTREAM "127.0.0.1:8092"`). Changing
> `--up-port` / `--proto-port` without editing those lines and rebuilding will
> make WFX unable to reach the mock; the runner detects this and aborts early
> with a clear message.

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | All vectors passed |
| `1` | Correctness failure, or the WFX worker died during the run |
| `2` | **Security** finding: a desync, response-smuggle, request-injection, auth-bypass, STARTTLS injection/downgrade, or multiplex-bleed defeat |

---

## Architecture

```
audit (endpoint_audit.py) --HTTP--> WFX app (/call, /inject)     --HTTP------> mock: http_upstream.py
        |                             WFX app (/proto/*)          --raw proto-> mock: proto listener
        |                             WFX app (/smtp/send, /inject) --SMTP----> mock: smtp_upstream.py (one listener per persona)
        +----------------- stages raw response bytes, reads counters ---------------+
```

- **`app/`**: a WFX project (scaffolded with `wfx new`), all in `src/main.cpp`.
  Its `/call` route turns each inbound request into an outbound `HttpEndpoint`
  call driven by `X-*` headers, and reflects the result back as JSON
  (`{"ep", "status", "bodylen", "body", "hdr"}`, where `ep` is the
  `EndpointStatus` int, `0 == SUCCESS`). `/inject` (POST) feeds the serializer
  hostile path/header bytes carried in the request body, the only way to smuggle
  raw CR/LF/NUL past the inbound parser. Up to three headers are forwarded via
  `X-Fwd` / `X-Fwd2` / `X-Fwd3` so dedup, ordering, and drop-among-clean-headers
  can be asserted. Nine `HttpEndpoint` instances give each test the knobs it
  needs: `default`, `small` (tiny header/body/count caps), `fast` (5s request
  timeout, the engine's floor, `connLimit=2` for pool-exhaustion tests),
  `coalesce` (dedup key set), `reuse` (single-slot pool, forces same-connection
  reuse so cross-request bleed is observable), and four connection-lifecycle
  probes: `dead` (nothing listening, connect refused), `unreach` (unrouteable
  TEST-NET address, connect times out), `idle` (5s idle timeout, the engine's
  floor), `prewarm` (opens 3 connections eagerly at boot).

  Separately, `/proto/call` drives four raw `WFX::Endpoint<>` instances
  (`good`/`bad`/`slow`/`reset`, one connLimit=1 slot each) against a tiny
  hand-rolled text protocol, to reach `onConnect`, `onDisconnect`, and
  multiplexing; `HttpEndpoint` can't exercise any of the three, since HTTP/1.1
  has no handshake and no concurrent-requests-per-connection. `/proto/disconnects`
  (+ `/reset`) exposes per-`DisconnectReason` counters set from the app's own
  `onDisconnect` callback.

  A third, separate pair drives `WFX::SmtpEndpoint`: `/smtp/send` (`X-Persona`
  selects one of `SmtpEndpointOf`'s 24 instances, one per `smtp_upstream.py`
  listener, `X-From`/`X-FromName`/`X-To`/`X-ToName`/`X-Subject`/`X-ReplyTo`
  headers feed `DataBody`, the POST body is the message body so it can carry
  raw CR/LF for dot-stuffing tests) drives a full `MAIL FROM`/`RCPT TO`/`DATA`
  transaction end to end and reflects the result as JSON (`{"ep", "stage",
  "code", "success"}`, `stage` is `"reserve"|"mail"|"rcpt"|"data_start"|
  "data_body"|"done"`, pinpointing which command the failure happened on).
  `/smtp/inject` (POST, `X-Field` selects which caller-supplied field the raw
  request body is fed into) is the only way to smuggle raw CR/LF/NUL past
  `SmtpTransaction`'s own typed methods, the same role `/inject` plays for
  HTTP.

- **`http_upstream.py`**: a deliberately thin, raw-socket **byte oracle**, with two
  listeners. The HTTP one (300+ attacks live in the audit; the mock just puts
  exact bytes on the wire) has these key primitives:
  - **staging**: the audit `POST`s an arbitrary raw response blob to
    `/ctl/stage`, then asks WFX to fetch `/raw/<id>`, and the mock replays those
    exact bytes. Powers all the status-line, header, chunk, EOF, and limit fuzzing.
  - **fragmented delivery**: staging with `X-Mode: drip` (N-byte pieces) or
    `X-Mode: split` (one split at an offset) dribbles the same bytes across
    recv() boundaries, exercising the client's incremental line-reassembly and
    body-resume paths that a single write() never reaches.
  - **`/reflectraw`**: hands back the *exact* request head WFX emitted (CR/LF
    shown as `|`) so the serializer's header order, dedup, Content-Length
    correctness, and absence of any smuggled line break are asserted byte-for-byte.

  Fixed routes (`/ok`, `/chunked/<k>`, `/evil/*`, `/coalesce*`, `/reflect`,
  `/reflectraw`, `/kacount`, ...) cover cases needing stable behavior or
  server-side counters.

  The second listener (`PROTO_UPSTREAM`, `127.0.0.1:8092`) speaks the raw
  protocol: one `AUTH <token>\n` handshake per connection (`good`/`slow` reply
  `OK`, `bad` replies `ERR`, `reset` drops the connection without replying),
  then any number of `REQ <id> <key>\n` requests answered `RES <id> <value>\n`
  from independent per-request threads, so, with more than one request in
  flight, replies are deliberately free to come back out of order, exactly what
  the multiplexing tests need. `/ctl/protoconns` (on the HTTP listener) counts
  connections accepted by this second listener.

- **`smtp_upstream.py`**: unlike `http_upstream.py`'s single staged-response
  model, hostility here is **one fixed listener per persona** (`SMTP_PERSONAS`
  in the audit, one port each, all compiled into `app/src/main.cpp` as
  separate `SmtpEndpoint` instances, see [`SMTP_PERSONAS`](endpoint_audit.py)
  for the full table). Each listener runs the real greeting/`EHLO`/`STARTTLS`/
  `EHLO`/`AUTH`/`MAIL`/`RCPT`/`DATA` state machine, with one option flipped:
  refuse `STARTTLS`, splice plaintext right after the go-ahead, serve a
  self-signed/hostname-mismatched/expired cert, fail `AUTH` on purpose, flood
  or drip-feed a response to induce a hang, or drop the connection at a named
  stage. A separate control listener (`127.0.0.1:8199`) answers `STATS
  <persona>` / `RESET <persona>` with per-persona counters (`handshakes`,
  `hs_fail`, `auth_ok`, `auth_fail`, `bodies`, ...), which is how the audit
  proves not just that WFX *reacted* correctly but that the mock's own side
  of the exchange got exactly as far as expected, no further.

---

## What gets attacked (phases)

| Phase | Focus | Sample vectors |
|-------|-------|----------------|
| **framing** | The whole *legal* matrix must be accepted | CL / chunked (ext, trailer, multi) / close-delimited / HTTP/1.0 / HEAD bodyless / 1x-8x 1xx / status passthrough 200-999 / 204 / 304 |
| **statusline** | Malformed first line rejected, not crashed | `HTTP/2.0`, `HTTP/1.2`, `HTTP/0.9`, `HTTP/11`, `HTTP/1,1`, `http/1.1`, `ICY`, `RTSP`, leading space, 1/2/4-digit codes, `+`/`-` codes, tab separators, empty line, bare proto |
| **headers** | Header framing / smuggling | obs-fold, leading-tab fold, no colon, empty name, space/tab before colon, dup-CL (equal vs differ), **CL+TE** both orders, non-numeric / hex / signed / overflow CL, TE `gzip`, `chunked,gzip`, `gzip,chunked`, `x-chunked`, accepted TE casings |
| **chunked** | Chunk-size and framing edges | upper/lower/zero-pad hex, extensions, trailers, non-hex, `0x`, empty size, u64-overflow size, negative, space/tab in size, bad terminator, short-data+EOF, 4 GiB-over-cap |
| **eof** | Truncation at *every* parser phase | EOF mid status-line / mid-headers / after headers / mid CL body / mid chunk-size / mid chunk-data / mid trailer / RST / zero-byte / huge-CL-then-EOF |
| **desync** | **Keep-alive poisoning and smuggling** (security) | 204-with-body, 304-with-body, HEAD-with-body, trailing smuggled response, pipelined responses, each followed by a burst of clean `/ok` that must all stay pristine |
| **serialize** | Request-side dedup and **injection** (security) | engine owns Host / CL / TE (caller dupes dropped, all casings), CL correctness for 0/1/100/1000-byte bodies, buffer-grow on a 6 KB header, and **rejection** of CR/LF/NUL in path and header name/value |
| **limits** | Boundary enforcement on `small` caps | header-block / single-line / header-count / body-CL / cumulative-chunk / close-delimited / trailer-count at-cap vs over-cap, plus the fixed 8x 1xx cap and 1xx-with-body |
| **resource** | Exhaustion, timeouts, coalescing | slow-headers / slow-body to `REQUEST_TIMEOUT`, 4-into-2 pool exhaustion resolves cleanly, coalesce dedup (16 to 1 backend hit) vs control (16 to 16), clone integrity (16 full 1 KB copies), error fan-out, **two-key no cross-delivery** |
| **fragmentation** | Incremental parser under recv() splitting | drip whole responses 1 byte at a time (CL / empty / chunked / ext+trailer), 1xx+final fragmented, **split at every byte offset** of a CL response, split inside chunk-size and chunk-data, big-body drip/split length integrity |
| **methods** | Every verb serializes correctly | GET/OPTIONS/DELETE bodyless (no CL), POST/PUT/PATCH body + CL via `/reflectraw`, all seven verbs round-trip, HEAD stays bodyless |
| **security** | Smuggling / poisoning / leaks / DoS | cross-request **bleed** (A to B to A body + header isolation), **trailer** and **1xx** header-leak hidden, CL-bounded body (extra bytes dropped), keep-alive poison via chunked-trailing / 204+TE / 304+TE / 204+hidden-CL / drip-204, CR/LF/NUL path+header injection breadth (plus passthrough of non-CRLF/NUL bytes), header-byte and header-count DoS caps, pipelined-burst first-only |
| **lifecycle** | Connection lifecycle: connect, idle, prewarm | connect-refused and connect-unreachable both error cleanly within budget (worker survives), reconnect after upstream close, keep-alive reuse (`/kacount` climbs on one pooled conn), idle timeout recycles the connection, prewarm opens its connections eagerly at boot |
| **protocol** | `onConnect` / `onDisconnect` / multiplexing (raw `WFX::Endpoint<>`) | **auth-bypass** check: rejected (`bad`) and dropped-mid-handshake (`reset`) connections must never serve a request, handshake timeout (`slow`) surfaces as `EpHandshakeTimeout` within budget, worker survives every failure, a good connection still works right after (no pool poisoning), `onDisconnect` counted per reason (handshake-timeout / error / idle), and **multiplexing cross-request bleed**: 12 concurrent requests on one connLimit=1 slot, deliberately resolved out of order, every caller must get back exactly its own value |

| **pinning** | Slot reservation (`Reserve` / `ReservedSlot`) | two pins never share a physical connection (`connId` proves it), a pinned slot is never handed to another caller mid-session, release paths (early, destructor-late, double) all leave the pool usable, and coalescing can never merge two pinned callers however identical their requests |
| **push** | Server-initiated messages on an idle slot (`onPush`) | pushes arriving while a request is in flight go to `parse()`, never `onPush`, a partial push parks instead of spinning or wedging the slot, a flood is drained incrementally rather than buffered whole, an undecodable push closes the slot and the endpoint recovers |
| **streaming** | Bounded-memory chunked delivery (`Stream` / `StreamHandle`) | both `CHUNK_READY` families (server-driven and fetch-driven), a checksum over 200 chunks proving no chunk is dropped or duplicated, **peak RSS tracks one chunk, not the total**, abandonment mid-stream reclaims the slot, and two concurrent streams never cross-deliver |
| **upgrade** | In-band TLS (`UpgradeToTLS`) | a server refusing the upgrade must fail closed when the protocol requires TLS, and upgrading an already-wrapped connection must be refused rather than wrapping twice. The plaintext-discard half of this surface needs a real handshake, so it lives in `tls_audit` |
| **smtp_handshake** | The full happy-path handshake and transaction | `good` persona round-trips a complete `MAIL`/`RCPT`/`DATA` transaction with a final 250, mock records a completed TLS handshake and successful AUTH, and a dot-stuffed body (a leading `.` line) survives client-side stuffing + mock-side un-stuffing byte-for-byte, not swallowed as the `DATA` terminator |
| **smtp_auth** | `AUTH PLAIN` / `AUTH LOGIN` selection and failure | `AUTH LOGIN` fallback succeeds when the server doesn't offer `PLAIN`, wrong credentials are refused at the mail stage with the mock recording the failed AUTH and never an AUTH success, and a server offering neither implemented mechanism is refused rather than the client guessing at CRAM-MD5 or similar |
| **smtp_starttls** | STARTTLS enforcement and **injection/downgrade** (security) | a server that never advertises `STARTTLS` never gets a plaintext `AUTH` fallback (mock never even sees a handshake attempt), the CVE-2011-0411/CVE-2026-41319-class `inject` persona (plaintext spliced in right after the `"220 Ready to start TLS"` go-ahead) fails closed and the injected bytes never reach an authenticated session, a spliced-response continuation line with a mismatched status code is refused rather than silently accepted, and a non-SMTP banner is refused |
| **smtp_certs** | STARTTLS certificate rejection (security) | untrusted self-signed, hostname-mismatched, and expired certs are all refused, with the mock confirming the client actually bailed at the TLS layer (a failed handshake recorded on the mock's side, not just a client-side error code) |
| **smtp_resource** | Handshake and mid-transaction hangs never block forever | a response flood, an oversized unterminated line, and byte-at-a-time trickling during the handshake all resolve via either `LineResponse`'s own line/line-count caps or `connectTimeoutSeconds`, whichever fires first; a mock that authenticates normally then goes silent forever on `DATA` is caught by `requestTimeoutSeconds` instead |
| **smtp_drops** | Abrupt disconnects at every handshake stage | a drop after the greeting, before the handshake, mid-STARTTLS, or mid-AUTH is refused cleanly at the mail stage; a drop after `DATA`'s `354` go-ahead (handshake and MAIL/RCPT already succeeded) is a live-connection failure, not a connect failure, and is refused without hanging |
| **smtp_inject** | CR/LF/NUL injection through every caller-supplied field (security) | `MailFrom`/`RcptTo`/`DataBody`'s `fromName`/`toName`/`subject`/`replyTo`/`body` fields and the operator-configured `heloName` are all screened and rejected at the serializer before a single byte reaches the wire, the same SMTP analogue of HTTP header/request-line injection `serialize`'s vectors cover for the HTTP path |

Vectors marked **security** in the report cause exit code `2` if they trip: a
tripped desync, smuggle, injection, bleed, leak, or auth-bypass means a
hostile upstream (or a malformed response) defeated the client.
