# WFX base audit

Phased correctness and security testing for the WFX server itself. Boots it, runs
the phases, stops it, then scans for sanitizer reports and force-killed workers.
Shared plumbing lives in `tests/common/`, see `tests/README.md`

---

## Requirements

- WFX installed with `./scripts/install.sh --local-debug` (puts `wfx` on `PATH`, its headers
  under `~/.wfx/src`, which `app/CMakeLists.txt` builds against, and ASan/UBSan on), or run
  through `tests/run_audits.sh` after that install
- Python 3.8+, standard library only
- Linux with `/proc` (for worker PID discovery)

---

## Quick start

```bash
cd tests/base_audit

# All phases
python3 base_audit.py

# Single phase
python3 base_audit.py --phase security
python3 base_audit.py --phase protocol
python3 base_audit.py --phase features
python3 base_audit.py --phase forms
python3 base_audit.py --phase query
python3 base_audit.py --phase cors
python3 base_audit.py --phase groups
python3 base_audit.py --phase metrics
python3 base_audit.py --phase soak
python3 base_audit.py --phase chaos

# Every WFX log line, not just crash dumps
python3 base_audit.py --wfx-logs all

# Different binary or port
python3 base_audit.py --wfx /path/to/wfx --port 9090

# GitHub Actions
python3 base_audit.py --ci
```

---

## Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--phase PHASE` | `all` | `all`, `security`, `logging`, `protocol`, `features`, `forms`, `query`, `cors`, `refusals`, `groups`, `metrics`, `soak`, `chaos` |
| `--host HOST` | `127.0.0.1` | Server hostname |
| `--port N` | `8080` | HTTP port |
| `--wfx PATH` | `wfx` | Path to the wfx binary |
| `--app-dir DIR` | `app` | App directory passed to `wfx run` |
| `--pid-file PATH` | `~/.wfx/daemons/app.pid` | WFX daemon PID file |
| `--ready-timeout N` | `20` | Seconds to wait for `/health` on startup |
| `--phase-timeout N` | `0` | Max seconds per phase before marking TIMEOUT and continuing (0 = unlimited; auto-set to 300 with `--ci`) |
| `--wfx-logs MODE` | `crash` | Stream WFX's own logs live: `off`, `crash` (dumps and sanitizer reports), `important` (those plus WRN/ERR/FTL), `all`. This suite provokes contract violations by the thousand, so its `[ERR]` lines are the tests working and printing them buries the results |
| `--ci` | off | CI-friendly output: no inline dot progress, GitHub Actions workflow commands (`::group::`, `::error::`) |
| `--list-phases` | n/a | Print available phases and exit |

---

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | All phases passed |
| `1` | A check failed, a phase timed out or raised, a sanitizer report was written, a worker ignored SIGTERM at shutdown, or the server was dead at the end |
| `2` | Security finding: path traversal, response splitting, info leak, cross-request body leak, form header injection or decode bypass, query value mangling, or a CORS origin leak. The run stops after the phase that produced it: later phases would be measuring an already-compromised server |
| `3` | WFX never answered `/health`, so nothing was exercised |

---

## Phases

### SECURITY

Runs with 16 background flood threads so the server is under load.
All vectors are retried up to 3 times. Findings are collected: no early bail.

- **Path traversal: URL** (109 vectors): `../../../../etc/passwd` and other
  sensitive targets across every encoding variant: plain `../`, `%2e%2e%2f`,
  double-percent `%252f`, overlong UTF-8 `%c0%af`, Unicode fullwidth `%ef%bc%8f`,
  backslash `%5c`, null-byte injection, semicolon bypass, dot-segment tricks,
  query/fragment appended, Windows-style paths.
- **Path traversal: X-File header** (58 vectors): same corpus delivered as the
  `X-File` header to `/download`. Also includes absolute paths and null-byte-truncated paths,
  and `/download` with no `X-File` at all must return 400.
- **CRLF / response splitting** (19 vectors): CR, LF, CRLF, null bytes, Unicode
  line terminators, full injected HTTP response blocks, reflected through `/echo`.
  A reflected vector is recorded only if `X-Injected` appears as a parsed header name.
  Two more checks record the same finding without it: a duplicated `Content-Type` or
  `Set-Cookie`, and a double CRLF faking a second response.
- **Info leakage**: 16 routes scanned for password-file entries, environment variables, keys
  and certificates, process maps and command lines, and source or config text.
- **Cross-request body leak**: on one keep-alive connection, a body-bearing `POST /echo-body`
  followed by a body-less one (`Content-Length: 0`); the second must echo nothing, proving a
  request's body view does not survive into the next request on the same connection.
- **Contract violations**: each `/violate/*` route is hit 10 times; every
  response must be 500 without crashing the worker.

Exit code 2 for traversal, response-splitting, info-leak or cross-request-body-leak findings; 1 for other failures.

---

### PROTOCOL

Zero concurrent load. Payloads sent serially; `/health` polled after each
corpus. No assertion on status codes: the server just has to stay alive.

- **Header abuse** (61 vectors): oversized values, header floods, line folding,
  bad header names, control bytes, duplicate `Host`, negative and overflow
  `Content-Length`, `TE+CL` together, hop-by-hop abuse.
- **Malformed** (43 vectors): truncated requests, bad HTTP versions, binary junk,
  TLS `ClientHello`, HTTP/2 preface, protocol blobs (SMTP, IRC, Redis), null
  in path, CRLF floods, overlong URIs and method tokens.
- **Methods** (22 vectors): `OPTIONS *`, `TRACE`, `CONNECT`, absolute-form
  targets, unknown methods, HTTP/1.0, WebDAV verbs.
- **Smuggling** (22 vectors): `CL+TE` conflicts, double `Content-Length`,
  malformed chunk sizes, obfuscated `Transfer-Encoding`, bare-LF lines,
  oversized chunk extensions.
- **Body bombs** (8 vectors): bodies at/over the 64 KiB limit, declared size
  mismatches, integer overflow variants in `Content-Length`.

Exit code 1 if the server fails `/health` after any corpus.

---

### FEATURES

Correctness check of every user-facing route, run with 8 background flood
threads. Each check retries up to 3 times. Wrong status code, missing body
content, or missing/wrong header = finding.

| Area | What is checked |
|------|----------------|
| Dynamic segments `:uint` | `/items/42`, `/items/0`, max u64, type mismatch → 404 |
| Dynamic segments `:int` | Negative values, zero |
| Dynamic segments `:string` | Basic strings, hyphenated names |
| Dynamic segments `:uuid` | Valid UUIDs echoed back; invalid → 404 |
| Group-prefixed paths | `/api/v1/status`, `/api/v1/item/<id>` |
| `MwContinue` | `X-Route-MW: hit` present on `/mw/injected` |
| `MwBreak` | `/mw/blocked` → 403, handler never runs |
| `MwSkipNext` | Second middleware skipped, handler runs |
| Context storage | MW writes `uid=42`, handler echoes it |
| Shared callable types | Two plain sync functions, two plain async functions and two closures from one middleware factory each run their own (`/fn/sync-a`, `/fn/sync-b`, `/fn/async-a`, `/fn/async-b`, `/mw/factory-a`, `/mw/factory-b`) |
| Async handler | `/async/sleep` returns `slept`; 20 concurrent hits |
| Inline async | Async middleware and routes that finish without suspending (`/mw/async-inline`, `/mw/async-inline-break`, `/async/inline`) |
| Throwing child task | A `WFX::Task` that throws reaches the coroutine awaiting it: `/async/child-throws` catches it (`caught`), the uncaught `/async/void-child-throws` fails the request with `500` |
| Pipelining | A request sent while the previous one is still suspended is never answered |
| Abandoned coroutines | The worker survives a client leaving mid-sleep on a nested coroutine |
| `ImJson` | `/json/im` body contains `"immediate"`, `"meta"` and a pushed string literal (`"tags":["admin"]`, not `true`) |
| `RmJson` | `/json/rm` body contains `"retained"` |
| JSON merge and overwrite | `/json/merge` and `/json/overwrite` produce the right bytes after their sources are gone |
| JSON parsing | Valid bodies → 200; invalid, truncated, empty or duplicate-key bodies → 400 |
| Chained headers | `X-Chain-A/B/C` all present with exact values on `/chain` |
| Metrics | `/metrics` returns 200 with a `crashes` field |
| Buffer relocation | `/echo-full` echoes path, header and body intact across a mid-parse read-buffer move |
| Limits | Body and header limits at the exact boundary |
| Drip | A request sent one byte at a time |
| Streaming | `/stream` reassembles exactly, and the connection closes on time |
| Flush | `/flush/single`, `/flush/zero`, `/flush/multi`, `/flush/no-end` and `/flush/heavy`, the last under real backpressure |
| Templates | static, dynamic, conditional, loop, include, inherit |
| Template branch isolation | Wrong branches must not appear in output |
| Template inheritance | Child block overrides base; base title must not leak |
| Content-Type | static, dynamic, loop, include and inherit return `text/html` |

Exit code 1 for any failure.

---

### FORMS

`WFX::Form` end to end, against `/form` (validators, required and optional
fields) and `/form/raw` (one unvalidated field, so a vector can be attributed to
decoding rather than to a validator). Four groups, each a run of vectors:

| Group | What it proves |
|-------|----------------|
| **functional** | Content-Type matching, field structure (order, count, duplicate keys, empty values), required-vs-optional, and validator bounds |
| **percent-decoding** | Every escape shape through a single isolated field: valid pairs, truncated escapes, non-hex digits, `+` handling, NUL and CRLF bytes, and over-long encodings |
| **header-injection** | A decoded value carrying CR/LF must never reach a response header. This is the security-relevant one: it is the path from attacker-controlled form input to response splitting |
| **dos-resistance** | Structural abuse: huge field counts and long runs of empty separators. Must be bounded and refused, never hang |

Validators bound the **decoded** length, not the raw wire bytes, so a percent-
encoded payload cannot slip past a length check by being longer before decoding
than after.

Exit code 2 if header injection lands or a double-decode or invalid escape is accepted, 1 for
other failures.

---

### QUERY

`Request::GetQueryParams()`: key lookup, raw (undecoded) values, duplicate keys
(first wins), and correct behavior with no query string, an empty one, or one made
entirely of empty segments.

This is also the regression suite for a real parser fix: the request line used to be
normalized (slash-collapsing, `.`/`..` resolution) as one blob covering both the path
and the query string, so a query value shaped like a path (`/foo/../bar`, `//evil`)
got silently mangled by traversal-defense logic that was never meant to touch it. The
parser now splits the query off before normalizing and reassembles it byte-for-byte
untouched afterward, so a set of traversal-shaped query values is checked to survive
completely unchanged. Also covered: no auto-decoding (`+` and `%20` stay literal,
same as header values), path normalization still working with a query attached, a
4000-byte query value, and a long query surviving a body that grows the receive buffer
under it.

Exit code 2 if a traversal-shaped value gets mangled, 1 for other failures.

---

### CORS

`CoreEngine::HandleCors` plus the generic `OPTIONS` `Allow:` fallback
(`CoreEngine::HandleGenericOptions`), tested against `app/config/wfx.local.toml`'s real `[CORS]`
section: two allowed origins, credentials on, `allowed_headers` empty (exercises the
reflect-the-request branch), `exposed_headers` set (exercises the static-list
branch). `wildcardOrigin` and the `"*"` + credentials load-time rejection aren't
reachable from a live request (they're config-load-only), so they're not covered
here.

The check list is grounded in real CORS misconfiguration classes, not just feature
coverage:

- **basic origin reflection** (PortSwigger "CORS vulnerability with basic origin
  reflection"): an unlisted `Origin` must get zero CORS headers, never an echoed
  `Access-Control-Allow-Origin`
- **trusted null origin** (CVE-2019-9580, StackStorm): `Origin: null` must not be
  implicitly trusted just because it looks like a special case
- **subdomain/prefix/suffix bypass** (PortSwigger's "trusted subdomains" lab class):
  WFX matches origins by exact string only, so near-miss origins (scheme, port,
  subdomain, case) must all fail closed rather than fuzzy-match
- **reflected-origin-plus-credentials** (CVE-2026-54290, Hono CORS middleware):
  credentials must never appear alongside an origin that was not actually matched
  against the allowlist
- **persistent-header survival**: CORS headers are written via
  `WritePersistentHeader` specifically so they outlive a later `AbortWithError`
  (404, etc.), asserted directly here

### Refusals

What the engine says when it turns a request away, rather than whether it survives one.

- **wrong method vs wrong path**: a method a known path doesn't define is `405` with an `Allow:`
  naming the ones it does (RFC 9110 15.5.6), and only a path with no route under any method is
  `404`. Answering `404` to both tells a caller its URL is wrong when its method is, and lets a
  prober map a path's methods by which refusal comes back
- **framing of the canned refusals**: the responses written whole to the socket, before there is
  a `Response` object to go through, declare a `Content-Length` by hand next to their body. Each
  reachable one is checked to send exactly what it declares: short, and a client waits on bytes
  that never arrive; long, and it reads the next response on that connection as part of this one

Exit code 2 if an origin-matching bypass or credential leak lands, 1 for other
failures.

---

### GROUPS

Route groups that don't pair up must stop the server from starting. Each case in
`GROUP_CASES` is the `wfx new` template, scaffolded into a temp dir, with its routes
replaced and a `/health` added, then run on the next port up from the app under test:

| Case | Must log, and never answer `/health` |
|------|--------------------------------------|
| `WFX_GROUP_START` with no `WFX_GROUP_END` | `WFX_GROUP_START() without a matching WFX_GROUP_END()` |
| `WFX_GROUP_END` with no `WFX_GROUP_START` | `WFX_GROUP_END() without a matching WFX_GROUP_START()` |

An open group would otherwise prefix every route registered after it. Exit code 1 on
any failure.

---

### METRICS

Drives known request counts through `/status/<code:uint>` (one status class each)
and `/health`, then confirms `/metrics` reflects exactly what was driven: per-route
request counts, status-class buckets, byte counters, and latency histogram samples,
with `/health` traffic never bleeding into `/status`'s counters or vice versa.
`status1xx` is unreachable over HTTP (no response's *final* status is 1xx), so it's
asserted to stay zero rather than driven. Runs before `chaos`, whose worker kills
reset a slot's counters mid-flight and would break a delta.

Exit code 1 for any mismatch.

### SOAK

2000 sequential requests over one keep-alive connection, rotating across five
routes with different body shapes, checking every response's status and body
match exactly. Regression coverage for read/write buffer reuse across many
requests on the same connection, not just a handful.

Exit code 1 on any mismatch or connection failure.

---

### CHAOS

6 scenarios. Each is followed by a recovery wait and the full `ROUTE_CHECKS` sweep (68 checks
over 53 distinct paths). The master process is never targeted: only worker children.

| # | Scenario | What happens |
|---|----------|-------------|
| 1 | **Single worker kill × 3** | SIGKILL a random worker, 8 flood threads during restart, verify recovery |
| 2 | **Kills under sustained load** | 16 load threads for 30 s; one worker killed every 5 s |
| 3 | **Rapid-fire kills** | 1 kill every 2 s × 5: stresses restart/backoff machinery |
| 4 | **Dual-worker kill** | Both workers killed simultaneously; both must restart and serve correctly |
| 5 | **SIGSTOP → SIGCONT** | One worker paused 3 s while 8 threads hammer; unpaused; routes verified |
| 6 | **Kill under mixed load** | 75 half-open idle connections + 12 concurrent large-response threads; one worker killed mid-flight |

The recovery wait is 15 s, 20 s for rapid-fire and dual-kill, and SIGSTOP has none since its
worker is never killed. Final `/metrics` is fetched and crash + restart counts are reported.

Exit code 1 if any scenario fails to recover or a correctness check fails.

---

## Test app (`app/`)

A minimal WFX project whose only purpose is to give the audit surface for
every user-facing feature.

| Route | Purpose |
|-------|---------|
| `/health` | Liveness probe |
| `/text` | Minimal 200: variety for flood workers |
| `/echo` | Reflects `X-Echo` header: CRLF injection target |
| `/echo-body` | Echoes POST body: body-bomb and smuggling target |
| `/echo-full` | Echoes path, a header and the body together: proves every `string_view` into the read buffer survives a mid-parse relocation |
| `/form` | `WFX::Form` with validators, required and optional fields |
| `/form/raw` | `WFX::Form` with one unvalidated field: isolates percent-decoding from validation |
| `/big` | 1 MiB of `A`: large-response target |
| `/stream` | Chunked 512 × 256 B stream: streaming path target |
| `/flush/single`, `/flush/zero`, `/flush/multi` | `Flush()` from inside a coroutine: one write then `FlushEnd()`, nothing written, and 200 rounds |
| `/flush/heavy` | ~8 MiB over 4000 flushed rounds, enough to force real backpressure |
| `/flush/no-end` | Forgets `FlushEnd()`: the response must still end in a valid chunk terminator |
| `/download` | Serves `public/<X-File>`: path-traversal target |
| `/violate/204body` | 204 with body: must return 500 |
| `/violate/conn` | Sets `Connection` header: engine-owned, must return 500 |
| `/violate/recommit` | Calls `Commit()` twice: must return 500 |
| `/violate/flush-after-body`, `/violate/flush-twice` | Flush API misuse: must return 500 |
| `/cors/multi` | One path with GET, POST and PUT and no OPTIONS handler: the generic `OPTIONS` `Allow:` fallback |
| `/query/echo` | Reports the raw value of `v` and how many query pairs were parsed |
| `/metrics` | Live crash / restart / RSS / per-route counters as JSON |
| `/status/<code:uint>` | Returns the given status code: `metrics` phase's status-class workhorse |
| `/items/<id:uint>` | Dynamic `:uint` segment |
| `/items/signed/<id:int>` | Dynamic `:int` segment (negative values) |
| `/greet/<name:string>` | Dynamic `:string` segment |
| `/uuid/<id:uuid>` | Dynamic `:uuid` segment |
| `/api/v1/status` | Group-prefixed flat path |
| `/api/v1/item/<id:uint>` | Group-prefixed path with dynamic segment |
| `/mw/injected`, `/mw/blocked`, `/mw/skipnext`, `/ctx` | Middleware/context targets, see the FEATURES table above |
| `/fn/sync-a`, `/fn/sync-b`, `/fn/async-a`, `/fn/async-b`, `/mw/factory-a`, `/mw/factory-b` | Callables sharing a type: plain functions of one signature, and closures from one middleware factory |
| `/mw/async-inline`, `/mw/async-inline-break` | Async middleware that finishes without suspending, continuing or breaking with 403 |
| `/async/inline` | Async route that responds without awaiting anything |
| `/async/sleep` | Async coroutine + `WFX::SleepFor(25ms)` |
| `/async/slow`, `/mw/async-slow` | An async handler or middleware suspended long enough for a pipelined request to arrive |
| `/async/nested-slow` | A route suspended inside a nested coroutine, for the abandoned-client check |
| `/async/child-throws`, `/async/void-child-throws` | A child `Task<WFX::String>` that throws, caught by its parent, and a child `Task<>` that throws, uncaught |
| `/json/im` | `WFX::ImJson` streaming JSON |
| `/json/rm` | `WFX::RmJson` DOM-then-serialise JSON |
| `/json/merge`, `/json/overwrite` | `RmJson` `Merge`/`Set` from a destroyed source, and scalars assigned over containers |
| `/parse-json` | `WFX::ParseJson` body parsing |
| `/chain` | Three chained `.Header()` calls |
| `/template/static` | Static template render |
| `/template/dynamic` | Template with runtime variable |
| `/template/cond/<n>` | Conditional branch isolation |
| `/template/loop` | Loop rendering |
| `/template/include` | Template include |
| `/template/inherit` | Template inheritance (child overrides block) |
