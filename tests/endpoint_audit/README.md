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
   raw-protocol listener on `127.0.0.1:8092` and its SP listener on
   `127.0.0.1:8093`,
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
| `--sp-port` | `8093` | Mock SP-upstream port, must match `SP_UPSTREAM` in `app/src/main.cpp` |
| `--wfx` | `wfx` | Path to the wfx binary |
| `--app-dir` | `app` | App directory passed to `wfx run` |
| `--ready-timeout` | `30` | Seconds to wait for `/health` |
| `--phase` | `all` | Run a single phase |
| `--list-phases` | n/a | Print phases and exit |
| `--wfx-logs` | `important` | Stream WFX worker/master logs live: `off`, `important` (WRN/ERR/FTL + crash keywords), or `all` |
| `--ci` | off | No colors; emit GitHub Actions `::group::`/`::error::` commands |

> All three mock ports are **compile-time baked** into the WFX app (`#define
> UPSTREAM "127.0.0.1:8091"` / `PROTO_UPSTREAM "127.0.0.1:8092"` / `SP_UPSTREAM
> "127.0.0.1:8093"`). Changing `--up-port` / `--proto-port` / `--sp-port`
> without editing those lines and rebuilding will make WFX unable to reach the
> mock; the runner detects this and aborts early with a clear message.

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | All vectors passed |
| `1` | Correctness failure, or the WFX worker died during the run |
| `2` | **Security** finding: a desync, response-smuggle, request-injection, auth-bypass, STARTTLS injection/downgrade, or multiplex-bleed defeat |

---

## Architecture

```
audit (endpoint_audit.py) --HTTP--> WFX app (/call, /inject)       --HTTP------> mock: http_upstream.py
        |                             WFX app (/proto/*)            --raw proto-> mock: proto listener
        |                             WFX app (/sp/*)                --SP-------> mock: SP listener
        |                             WFX app (/smtp/send, /inject) --SMTP------> mock: smtp_upstream.py (one listener per persona)
        +----------------- stages raw response bytes, reads counters -----------------+
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

  A third raw `WFX::Endpoint<>` family, the "SP" protocol, is deliberately
  **not** multiplexed (unlike `/proto/*` above), so it can reach the three
  things the multiplexed path structurally can't: `/sp/get` and `/sp/stream`
  drive plain calls and `Stream()` (both `CHUNK_READY` families), `/sp/reserve`
  and `/sp/reserve/pair` / `/sp/reserve/stream` drive `Reserve()`/`ReservedSlot`
  pinning, `/sp/push` (+ `/sp/push/reset`) exposes counters for unsolicited
  `onPush` messages, and `/sp/abort` (+ `/sp/abort/reset`) drives the
  graceful-cancel path. `/sp/rss` reports the process's resident memory, used
  by the streaming phase to prove peak memory tracks one chunk, not the total.

  A fourth, separate pair drives `WFX::SmtpEndpoint`: `/smtp/send` (`X-Persona`
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

- **`http_upstream.py`**: a deliberately thin, raw-socket **byte oracle** with
  three listeners.
  - The **HTTP listener** (port 8091) just puts exact bytes on the wire; the
    300+ attacks all live in the audit itself, not the mock. Its key trick is
    **staging**: `POST /ctl/stage` an arbitrary raw response, then have WFX
    fetch `/raw/<id>`, optionally dripped or split mid-write (`X-Mode:
    drip`/`split`) to exercise incremental parsing. `/reflectraw` hands back
    the exact request bytes WFX sent, for byte-for-byte serializer checks.
    See the script's own comments for the full route list.
  - The **raw-protocol listener** (port 8092) speaks a tiny hand-rolled
    `AUTH`/`REQ`/`RES` text protocol with replies deliberately returned out of
    order, for the multiplexing tests.
  - The **SP listener** (port 8093) speaks a similar line protocol plus a
    `STARTTLS`/`AUTH` handshake, `STREAM`/`PAGE` request kinds, and
    unsolicited `PUSH` replies; every reply echoes `connId` so the harness can
    prove which physical connection served a request.
- **`smtp_upstream.py`**: one fixed listener per persona (`SMTP_PERSONAS` in
  `endpoint_audit.py`, one port each, each wired to its own `SmtpEndpoint` in
  `app/src/main.cpp`), each running the real handshake state machine with one
  thing flipped (refuse STARTTLS, inject plaintext, bad cert, fail AUTH, hang,
  or drop at a named stage). A control listener (port 8199) answers `STATS
  <persona>`/`RESET <persona>` with per-persona counters, so the audit can
  confirm the mock's side of the exchange got exactly as far as expected.

---

## What gets attacked (phases)

Run `python3 endpoint_audit.py --list-phases` to get this list straight from the
script; re-check it against the table below whenever a phase is added or
removed, this table has fallen behind before.

| Phase | Focus |
|-------|-------|
| **framing** | The whole *legal* response matrix (CL, chunked, close-delimited, HTTP/1.0, HEAD, 1xx, 204/304) must be accepted |
| **statusline** | Malformed status lines are rejected, never crash the parser |
| **headers** | Header framing and smuggling: obs-fold, dup-`Content-Length`, **CL+TE together**, bad `Transfer-Encoding` casings |
| **chunked** | Chunk-size and chunk-framing edge cases, including overflow and malformed terminators |
| **eof** | Truncation at every parser phase (mid status-line, mid-header, mid-body, RST, zero-byte) |
| **desync** (security) | Keep-alive poisoning and response smuggling; a clean burst after a hostile response must stay pristine |
| **serialize** (security) | Request-side header dedup, correct `Content-Length`, and rejection of CR/LF/NUL in path/headers |
| **limits** | Boundary enforcement on the `small` endpoint's header/body/count caps, at-cap vs over-cap |
| **resource** | Timeouts, pool exhaustion, coalescing dedup, clone integrity, cross-key isolation |
| **fragmentation** | Incremental parsing under `recv()` splitting, down to a 1-byte drip and every split offset |
| **methods** | Every HTTP verb serializes correctly, bodyless verbs stay bodyless |
| **security** | Cross-request bleed, trailer/1xx header leaks, keep-alive poisoning variants, injection breadth, DoS caps |
| **lifecycle** | Connect-refused/unreachable, reconnect, keep-alive reuse, idle timeout, prewarm |
| **protocol** | `onConnect`/`onDisconnect`/multiplexing on the raw proto endpoint: auth-bypass, handshake timeout, cross-request bleed under multiplexing |
| **soak** | 2500 sequential multiplexed requests must all succeed and stay on one pooled connection |
| **pinning** | `Reserve()`/`ReservedSlot`: two pins never share a connection, release paths all leave the pool usable, coalescing never merges pinned callers |
| **push** | `onPush` on an idle slot: in-flight bytes never misroute to it, partial pushes park instead of spinning, a flood drains incrementally |
| **streaming** | `Stream()`/`StreamHandle`: both chunk families, no dropped/duplicated chunks, peak RSS tracks one chunk not the total |
| **upgrade** | In-band `UpgradeToTLS`: a refused upgrade fails closed, double-wrapping an already-secure slot is refused (the plaintext-discard half needs a real handshake and lives in `tls_audit` instead) |
| **metrics** | `/metrics` counters match driven calls exactly: requests, completions, status class, latency histograms, in-use gauge quiescent at rest |
| **abort** | `onAbort` graceful-cancel via the SP protocol: cancel delivered without force-closing the connection, no leaked response on a late cancel, mid-stream abort, cancel-vs-timeout race |
| **smtp_handshake** | Full happy-path `EHLO`/`STARTTLS`/`AUTH`/`MAIL`/`RCPT`/`DATA`, including dot-stuffing round-tripping byte-for-byte |
| **smtp_auth** | `AUTH PLAIN`/`AUTH LOGIN` selection, wrong-credential refusal, and refusing a server offering neither mechanism |
| **smtp_starttls** (security) | STARTTLS enforcement, the CVE-2011-0411/CVE-2026-41319-class injection/downgrade persona, mismatched continuation codes |
| **smtp_certs** (security) | STARTTLS certificate rejection: untrusted, hostname-mismatched, and expired certs all refused |
| **smtp_resource** | Handshake and mid-transaction hangs resolve via line/line-count caps or the connect/request timeout, never block forever |
| **smtp_drops** | Abrupt disconnects at every handshake stage refuse cleanly, without hanging |
| **smtp_inject** (security) | CR/LF/NUL injection through every caller-supplied field and `heloName`, screened before a byte reaches the wire |

Phases marked **security** cause exit code `2` if they trip: a tripped desync,
smuggle, injection, bleed, leak, or auth-bypass means a hostile upstream (or a
malformed response) defeated the client. See the phase's own assertions in
`endpoint_audit.py` (search for `def phase_<name>`) for the exact vectors,
this table is a map to the code, not a substitute for reading it.
