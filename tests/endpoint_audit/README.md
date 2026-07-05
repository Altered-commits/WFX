# WFX Endpoint Audit

Adversarial correctness + security testing for the **outbound** HTTP/1.1 client —
`WFX::HttpEndpoint` and the raw `WFX::Endpoint<>` primitive in
`include/wfx/endpoint/http.hpp`.

Where `tests/audit` tortures WFX as an inbound **server**, this suite tortures it
as an HTTP **client**: it stands up a fully hostile upstream and fires 300+ attack
vectors *through* WFX at it, asserting the client-side parser/serializer never
crashes, never hangs past its timeout, never mis-frames one response into the
next, never smuggles a request, never leaks one request's data into another, and
never lets a malicious upstream poison a pooled keep-alive connection.

The suite deliberately thinks like an attacker: beyond the legal-matrix and
malformed-input corpora, it drives request smuggling / CRLF injection, keep-alive
**poisoning** (via oversized bodies, chunked framing, and no-body statuses), trailer
and 1xx **header-leak** probes, cross-request **bleed** on a reused connection,
coalescing cross-delivery (info-disclosure), and DoS caps (header-byte /
header-count amplification). Every byte the client puts on the wire can also be
inspected verbatim to prove no line break was smuggled into a serialized request.

Everything is coordinated by the `.py` runner — exactly like `tests/audit`, you
never launch the WFX server yourself.

---

## How to run

```bash
cd tests/endpoint

# Everything
python3 endpoint_audit.py

# One phase
python3 endpoint_audit.py --phase statusline
python3 endpoint_audit.py --phase desync

# List phases
python3 endpoint_audit.py --list-phases
```

The runner:

1. launches `upstream.py` (the hostile mock) on `127.0.0.1:8091`,
2. `wfx run app --detach` (builds + boots the client app),
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
| `--up-port` | `8091` | Mock upstream port — **must** match `UPSTREAM` in `app/src/main.cpp` |
| `--wfx` | `wfx` | Path to the wfx binary |
| `--app-dir` | `app` | App directory passed to `wfx run` |
| `--ready-timeout` | `30` | Seconds to wait for `/health` |
| `--phase` | `all` | Run a single phase |
| `--list-phases` | — | Print phases and exit |

> The mock port is **compile-time baked** into the WFX app (`#define UPSTREAM
> "127.0.0.1:8091"`). Changing `--up-port` without editing that line and
> rebuilding will make WFX unable to reach the mock — the runner detects this and
> aborts early with a clear message.

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | All vectors passed |
| `1` | Correctness failure, or the WFX worker died during the run |
| `2` | **Security** finding — a desync / response-smuggle / request-injection defeat |

---

## Architecture

```
harness (endpoint_audit.py)  ──HTTP──►  WFX app (/call, /inject)  ──HTTP──►  mock (upstream.py)
        │                                                                        ▲
        └────────────── stages raw response bytes, reads counters ───────────────┘
```

- **`app/`** — a WFX project (scaffolded with `wfx new`). Its `/call` route turns
  each inbound request into an outbound `HttpEndpoint` call driven by `X-*`
  headers, and reflects the result back as JSON
  (`{"ep", "status", "bodylen", "body", "hdr"}`, where `ep` is the
  `EndpointStatus` int, `0 == SUCCESS`). `/inject` (POST) feeds the serializer
  hostile path/header bytes carried in the request body — the only way to smuggle
  raw CR/LF/NUL past the inbound parser. Up to three headers are forwarded via
  `X-Fwd` / `X-Fwd2` / `X-Fwd3` so dedup, ordering, and drop-among-clean-headers
  can be asserted. Nine endpoint instances give each test the knobs it needs:
  `default`, `small` (tiny header/body/count caps), `fast` (5s request timeout —
  the engine's floor, `connLimit=2` for pool-exhaustion tests), `coalesce` (dedup
  key set), `reuse` (single-slot pool — forces same-connection reuse so
  cross-request bleed is observable), and four connection-lifecycle probes:
  `dead` (nothing listening, connect refused), `unreach` (unrouteable TEST-NET
  address, connect times out), `idle` (5s idle timeout — the engine's floor),
  `prewarm` (opens 3 connections eagerly at boot).

- **`upstream.py`** — a deliberately thin, raw-socket **byte oracle**. The 300+
  attacks live in the harness; the mock just puts exact bytes on the wire. Its
  key primitives:
  - **staging** — the harness `POST`s an arbitrary raw response blob to
    `/ctl/stage`, then asks WFX to fetch `/raw/<id>`, and the mock replays those
    exact bytes. Powers all the status-line / header / chunk / EOF / limit fuzzing.
  - **fragmented delivery** — staging with `X-Mode: drip` (N-byte pieces) or
    `X-Mode: split` (one split at an offset) dribbles the same bytes across
    recv() boundaries, exercising the client's incremental line-reassembly and
    body-resume paths that a single write() never reaches.
  - **`/reflectraw`** — hands back the *exact* request head WFX emitted (CR/LF
    shown as `|`) so the serializer's header order, dedup, Content-Length
    correctness, and absence of any smuggled line break are asserted byte-for-byte.

  Fixed routes (`/ok`, `/chunked/<k>`, `/evil/*`, `/coalesce*`, `/reflect`,
  `/reflectraw`, `/kacount`, ...) cover cases needing stable behaviour or
  server-side counters.

---

## What gets attacked (phases)

| Phase | Focus | Sample vectors |
|-------|-------|----------------|
| **framing** | The whole *legal* matrix must be accepted | CL / chunked (ext, trailer, multi) / close-delimited / HTTP/1.0 / HEAD bodyless / 1×–8× 1xx / status passthrough 200…999 / 204 / 304 |
| **statusline** | Malformed first line rejected, not crashed | `HTTP/2.0`, `HTTP/1.2`, `HTTP/0.9`, `HTTP/11`, `HTTP/1,1`, `http/1.1`, `ICY`, `RTSP`, leading space, 1/2/4-digit codes, `+`/`-` codes, tab separators, empty line, bare proto |
| **headers** | Header framing / smuggling | obs-fold, leading-tab fold, no colon, empty name, space/tab before colon, dup-CL (equal vs differ), **CL+TE** both orders, non-numeric / hex / signed / overflow CL, TE `gzip`, `chunked,gzip`, `gzip,chunked`, `x-chunked`, accepted TE casings |
| **chunked** | Chunk-size + framing edges | upper/lower/zero-pad hex, extensions, trailers, non-hex, `0x`, empty size, u64-overflow size, negative, space/tab in size, bad terminator, short-data+EOF, 4 GiB-over-cap |
| **eof** | Truncation at *every* parser phase | EOF mid status-line / mid-headers / after headers / mid CL body / mid chunk-size / mid chunk-data / mid trailer / RST / zero-byte / huge-CL-then-EOF |
| **desync** | **Keep-alive poisoning & smuggling** (security) | 204-with-body, 304-with-body, HEAD-with-body, trailing smuggled response, pipelined responses — each followed by a burst of clean `/ok` that must all stay pristine |
| **serialize** | Request-side dedup + **injection** (security) | engine owns Host / CL / TE (caller dupes dropped, all casings), CL correctness for 0/1/100/1000-byte bodies, buffer-grow on a 6 KB header, and **rejection** of CR/LF/NUL in path and header name/value |
| **limits** | Boundary enforcement on `small` caps | header-block / single-line / header-count / body-CL / cumulative-chunk / close-delimited / trailer-count at-cap vs over-cap, plus the fixed 8× 1xx cap and 1xx-with-body |
| **resource** | Exhaustion, timeouts, coalescing | slow-headers / slow-body → `REQUEST_TIMEOUT`, 4-into-2 pool exhaustion resolves cleanly, coalesce dedup (16→1 backend hit) vs control (16→16), clone integrity (16 full 1 KB copies), error fan-out, **two-key no cross-delivery** |
| **fragmentation** | Incremental parser under recv() splitting | drip whole responses 1 byte at a time (CL / empty / chunked / ext+trailer), 1xx+final fragmented, **split at every byte offset** of a CL response, split inside chunk-size & chunk-data, big-body drip/split length integrity |
| **methods** | Every verb serializes correctly | GET/OPTIONS/DELETE bodyless (no CL), POST/PUT/PATCH body + CL via `/reflectraw`, all seven verbs round-trip, HEAD stays bodyless |
| **security** | Smuggling / poisoning / leaks / DoS | cross-request **bleed** (A→B→A body + header isolation), **trailer** & **1xx** header-leak hidden, CL-bounded body (extra bytes dropped), keep-alive poison via chunked-trailing / 204+TE / 304+TE / 204+hidden-CL / drip-204, CR/LF/NUL path+header injection breadth (+ passthrough of non-CRLF/NUL bytes), header-byte & header-count DoS caps, pipelined-burst first-only |
| **lifecycle** | Connection lifecycle: connect / idle / prewarm | connect-refused and connect-unreachable both error cleanly within budget (worker survives), reconnect after upstream close, keep-alive reuse (`/kacount` climbs on one pooled conn), idle timeout recycles the connection, prewarm opens its connections eagerly at boot |

Vectors marked **security** in the report cause exit code `2` if they trip — a
tripped desync / smuggle / injection / bleed / leak means a hostile upstream (or a
malformed response) defeated the client.
