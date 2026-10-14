# WFX endpoint audit

Adversarial correctness and security testing for the raw `WFX::Endpoint<>`
primitive (`include/wfx/endpoint/base.hpp`): the connection pool, the handshake,
the serialize/parse loop, and everything layered on top of them.

This is the layer *underneath* the shipped protocol clients. `tests/client_audit`
attacks `HttpEndpoint`, `SmtpEndpoint` and `PostgresEndpoint` as protocols; this
suite attacks the machinery all three are built on, with two protocols written for
the job.

---

## Why two protocols

Half the primitive's surface is unreachable through a multiplexed endpoint, so
the app declares two protocols and the mock speaks both.

| | `mux` | `solo` |
|---|---|---|
| Multiplexed | yes, `hasCapacity` is set | no, `hasCapacity` is null |
| Port | 8092 | 8093 |
| Drives | `onConnect`, `onDisconnect`, several requests over one connection | `Reserve`, `Stream`, `onPush`, `onAbort`, `UpgradeToTLS` |

A non-null `hasCapacity` is what the engine reads to pick the multiplexed receive
loop, and it reads it on every receive, so `mux` is on that path permanently.
Three consequences follow, all of them in `os_specific/linux/epoll_connection.cpp`:

- `Reserve` on a multiplexed endpoint returns an empty handle. Multiplexing already
  shares one connection across concurrent requests, so pinning has nothing to add
  and the two would fight over slot ownership.
- The multiplexed receive loop has no chunk case at all. A `parse` that returns
  `CHUNK_READY` there falls through to the error branch and fails the slot, so
  `Stream` only works on a single-slot endpoint.
- `onAbort` never runs for a multiplexed request. A client that disconnects has
  only its own stream dropped from the slot's pending map, while the shared
  connection and every other stream in flight on it carry on untouched.

`onPush` is the one exception: it does reach a multiplexed slot, but only while
that slot has nothing in flight, which a phase driving concurrent requests at it
can never arrange on demand. `UpgradeToTLS` runs inside `onConnect` and is
reachable from either, so it lives on `solo` because that is where the handshake
with a `STARTTLS` step is.

Both protocols are line-oriented ASCII, written out above their listener in
`primitive_upstream.py` and mirrored above their section in `app/src/main.cpp`.

---

## Scope: this suite vs the others

**`tests/client_audit` owns the shipped protocol clients.** HTTP/1.1 framing,
SMTP handshakes, request smuggling, certificate rejection at the SMTP layer, the
Postgres SSLRequest/SCRAM/extended-query surface and per-protocol ALPN all live
there, none of it here.

**`tests/tls_audit` owns the certificate and trust surface.** Its `HttpEndpoint`s
are TLS from the first byte (`EpTlsRequire`), its one in-band `UpgradeToTLS`
endpoint starts plaintext and needs a working handshake, and it needs `mkcert`
plus a CA-trust step. This suite stays plaintext and needs no certificates at all.

`UpgradeToTLS` is split across this suite and `tls_audit`, because its vectors
have different needs:

| Vector | Suite | Why |
|---|---|---|
| Server refuses to upgrade and the protocol requires TLS, so it must fail closed | **endpoint** | No handshake is needed, the connection never becomes TLS |
| Server agrees, then sends garbage instead of a ServerHello | **endpoint** | The handshake is *meant* to fail, so no valid cert is required |
| Pre-upgrade plaintext must be discarded at the boundary | **tls** | Needs a *working* handshake to establish the boundary those bytes cross |
| `UpgradeToTLS` on an already-secure slot must be refused | **tls** | Needs a slot the engine already wrapped, so a real TLS endpoint |

The rule of thumb: if a vector needs a handshake to *succeed*, it belongs in
`tls_audit`; if it only needs one to be *attempted*, it belongs here.

---

## Architecture

```
audit (endpoint_audit.py) --HTTP--> WFX app /mux/*   --mux protocol--> mock :8092
                          --HTTP--> WFX app /solo/*  --solo protocol-> mock :8093
                                    (X-Solo: abortgated) ------------> mock :8094 (gate)
                          --HTTP-------------------------------------> mock :8091 (counters)
```

The audit never speaks either protocol itself. It drives WFX, WFX drives the mock,
and the mock's own counters are read back over a small HTTP control plane so
claims like "they shared one connection" or "the cancel named *that* connection"
are observed rather than assumed.

**`app/`** is a WFX project whose routes each turn one inbound request into one
outbound call and reflect the result as JSON. `/mux/call` and `/solo/get` make a
single call. `/solo/stream` drains a stream, and `/solo/reserve`,
`/solo/reserve/pair` and `/solo/reserve/stream` cover pinning. `/mux/disconnects`,
`/solo/push` and `/solo/abort` expose counters the app's own callbacks maintain,
which is what separates "the callback never ran" from "it ran and its work
failed". `/rss` reports worker resident memory, and that is what turns the
bounded-memory guarantee into something assertable.

**`primitive_upstream.py`** runs four listeners: the `mux` protocol, the `solo`
protocol, `gate` (the solo protocol behind an accept queue the audit can fill, so a
connect stays in flight until its SYN retry), and an HTTP control plane serving
`/ctl/ping`, `/ctl/mux/conns`, `/ctl/mux/hanging`, `/ctl/solo/cancels`,
`/ctl/solo/cancels/reset`, `/ctl/gate/close`, `/ctl/gate/open` and
`/ctl/gate/requests`. It is a fixture rather than a server, so every hostile thing
it does is scripted by the handshake token, the request key or a control-plane
call the audit picked.

---

## Run

```bash
cd tests/endpoint_audit

python3 endpoint_audit.py                        # everything
python3 endpoint_audit.py --phase solo_streaming # one phase
python3 endpoint_audit.py --list-phases          # what phases exist
python3 endpoint_audit.py --wfx-logs all         # stream every WFX log line
python3 endpoint_audit.py --ci                   # no colors, GitHub Actions groups
```

The runner launches the mock, boots the app with `wfx run app --port 8080
--detach`, drives every vector through WFX, leaves the `exit` checks' work in
flight, stops both, and prints a per-phase report.

### Requirements

- `wfx` on `PATH` (or `--wfx /path/to/wfx`)
- Python 3.8+, standard library only
- Linux

### Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--host` | `127.0.0.1` | Host for both WFX and the mock |
| `--port` | `8080` | WFX inbound port |
| `--control-port` | `8091` | Mock control plane, read by this suite only |
| `--mux-port` | `8092` | Mock `mux` listener, must match `MUX_UPSTREAM` in `app/src/main.cpp` |
| `--solo-port` | `8093` | Mock `solo` listener, must match `SOLO_UPSTREAM` in `app/src/main.cpp` |
| `--gate-port` | `8094` | Mock `gate` listener, must match `GATE_UPSTREAM` in `app/src/main.cpp` |
| `--wfx` | `wfx` | Path to the wfx binary |
| `--app-dir` | `app` | App directory passed to `wfx run` |
| `--ready-timeout` | `30` | Seconds to wait for `/health` |
| `--phase` | `all` | Run a single phase |
| `--list-phases` | n/a | Print phases and exit |
| `--wfx-logs` | `important` | Stream WFX logs live: `off`, `crash`, `important`, `all` |
| `--ci` | off | No colors, GitHub Actions `::group::` and `::error::` commands |

> The three mock ports are **compile-time baked** into the app (`#define
> MUX_UPSTREAM "127.0.0.1:8092"`, `SOLO_UPSTREAM "127.0.0.1:8093"` and
> `GATE_UPSTREAM "127.0.0.1:8094"`). Passing a different one on the command line
> without also editing those lines and rebuilding leaves WFX unable to reach the
> mock. The runner warns about any mismatch at startup. Only a `--mux-port`
> mismatch is also caught by the preflight call, which names `MUX_UPSTREAM` so the
> run reports it once; a wrong `--solo-port` or `--gate-port` shows up as failures
> in the phases that use it.

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | All vectors passed |
| `1` | A non-security check failed (a sanitizer report or a worker force-killed at shutdown included), the worker was not answering at the end of the run, or nothing was checked |
| `2` | **Security** finding, listed under Phases below |
| `3` | WFX never answered `/health`, so nothing was exercised |

---

## Phases

Run `python3 endpoint_audit.py --list-phases` to get this list straight from the
script, and re-check it against the table whenever a phase is added or removed.

| Phase | What it proves |
|-------|----------------|
| **mux_handshake** | `onConnect` gates the connection: a rejected, dropped or stalled handshake fails cleanly and is never served, a stall surfaces as a handshake timeout rather than a hang (stuck in a `Receive` or in a `Send` the server stopped reading), `onDisconnect` reports the reason that actually happened, a handshake whose `Send`s total more than the send buffer still succeeds, a multiplexed connect without `onConnect` that times out fails its waiting stream as a handshake timeout and counts as a connect failure rather than a background reconnect, and none of it poisons the next connection |
| **mux_multiplex** | Twelve concurrent requests answered deliberately out of order all share one physical connection, and each caller gets back its own value, matched by stream id and never by arrival order. An endpoint with `maxConcurrentStreams` 2 never puts more than two requests on one connection, and requests past that open more connections and are still served. Then, with the connection idle, `onDisconnect` reports an idle timeout, and a stream whose reply outlasts `requestTimeoutSeconds` fails as a request timeout, while `Reserve` there hands back an empty handle |
| **mux_soak** | 2500 sequential requests, then 30 concurrent waves of 16, all matched, all on one pooled connection. Hammers the pending-stream map and the per-stream parse state, where a leak or a use-after-free on the completion path shows up |
| **solo_pinning** | `Reserve`/`ReservedSlot`: every request on one reservation lands on the same connection, two live reservations never share one, byte-identical payloads on two pins still come back from those two connections, all three release patterns leave the pool usable, a send through a released reservation fails with `EpInvalidKey` instead of using the pool, a run of reservations longer than the pool never starves it, and a stream through a pinned slot stays on that slot |
| **solo_push** | `onPush` on an idle slot: an in-flight push is consumed by `parse` and never routed to `onPush`, a partial push parks instead of spinning, an undecodable one is rejected and recovered from, and a 2000-message flood drains incrementally rather than buffering |
| **solo_streaming** | `Stream`/`StreamHandle`: both chunk families deliver identical bytes, an abandoned stream releases its slot, an empty stream terminates, four concurrent streams never cross-deliver, and peak memory tracks one chunk rather than the total across 200x more data |
| **solo_upgrade** | In-band `UpgradeToTLS`: a refused upgrade fails closed instead of continuing in plaintext, a garbage ServerHello fails cleanly without hanging, and neither poisons the endpoint (the plaintext-discard half needs a real handshake, so it lives in `tls_audit`). Then it restarts with an `outbound_ca_path` that can't load: the outbound TLS context the first upgrade builds fails that upgrade rather than the worker, and later upgrades fail without building it again. It restarts once more on the baseline before the phase ends |
| **solo_abort** | `onAbort` graceful cancel: the cancel names the connection being aborted and leaves it open, an endpoint with no aux capacity fails gracefully, a leaked side connection is reclaimed by `connectTimeoutSeconds`, closing one three times never corrupts the aux pool, a side connection the server closes fails the pending `Receive` and frees its slot before `onAbort` resumes, one endpoint's aux pool never blocks another's, `onAbort` fires neither while the TCP connect or `onConnect` is still running (with or without an `onConnect`) nor once a request is already streaming, nothing of a request abandoned mid-connect reaches the backend, a 20-abort soak returns the in-use gauge to zero, and a silent backend still gets its cancel and is closed by `requestTimeoutSeconds` |
| **metrics** | `/metrics` counters match what was driven: requests, completions, latency samples, bytes both ways, every failure landing in a bucket rather than being dropped (a connection reset mid-request exactly once, in `other_errors`), an idle reservation reaching its idle timeout counted as no request timeout, and the in-use slot gauge quiescent at rest and back to zero afterwards |
| **exit** | Not a selectable phase: `before_stop` runs it right before the stop on every run. It leaves a coroutine suspended in each place only the worker's exit can reach (a slot inside `onConnect`, a handler awaiting its endpoint, an `onAbort` awaiting a side connection still connecting). The sanitizer scan after the stop proves exit destroyed them |

A check that fails and is marked **security** forces exit code `2`. In this suite
those are: one caller receiving another caller's response, a refused handshake
still being served, a pinned connection shared with anyone else, a send through a
released reservation running on a pooled connection, an unsolicited push
desyncing the framing or reaching `onPush`, a partial push spinning, the endpoint
staying poisoned after a rejected push, an abandoned stream or a failed upgrade,
a failed TLS upgrade leaving the endpoint usable in plaintext, memory growing
with the total response size rather than the chunk size, `onAbort` firing
where it must not, a cancel naming the wrong connection, an abandoned request or
its answer reaching the backend or the next caller, a repeated `Close` crashing
the worker, and one endpoint's aux slot blocking another's. Read the assertions
themselves in `endpoint_audit.py` (search for `def phase_<name>`) for the exact
vectors. This table maps to the code, it does not replace reading it.
