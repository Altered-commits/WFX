# WFX base audit

Phased correctness and security testing for the WFX server itself. Boots it, runs
the phases, stops it. Shared plumbing lives in `tests/common/`, see `tests/README.md`

---

## Requirements

- `wfx` on `PATH` (or pass `--wfx /path/to/wfx`)
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
python3 base_audit.py --phase features
python3 base_audit.py --phase forms
python3 base_audit.py --phase query
python3 base_audit.py --phase cors
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
| `--phase PHASE` | `all` | `all`, `security`, `protocol`, `features`, `forms`, `query`, `cors`, `metrics`, `soak`, `chaos` |
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
| `1` | Correctness failure, server crash, or server death |
| `2` | Security finding (path traversal or response splitting). The run stops at the first one: later phases would be measuring an already-compromised server |
| `3` | WFX never answered `/health`, so nothing was exercised |

---

## Phases

### SECURITY

Runs with 4 background flood threads so the server is under load.
All vectors are retried up to 3 times. Findings are collected: no early bail.

- **Path traversal: URL** (109 vectors): `../../../../etc/passwd` and other
  sensitive targets across every encoding variant: plain `../`, `%2e%2e%2f`,
  double-percent `%252f`, overlong UTF-8 `%c0%af`, Unicode fullwidth `%ef%bc%8f`,
  backslash `%5c`, null-byte injection, semicolon bypass, dot-segment tricks,
  query/fragment appended, Windows-style paths.
- **Path traversal: X-File header** (58 vectors): same corpus delivered as the
  `X-File` header to `/download`. Also includes absolute paths and null-byte-truncated paths.
- **CRLF / response splitting** (19 vectors): CR, LF, CRLF, null bytes, Unicode
  line terminators, full injected HTTP response blocks, reflected through `/echo`.
  A finding is recorded only if `X-Injected` appears as a parsed header name: zero false positives.
- **Contract violations**: each `/violate/*` route is hit 10 times; every
  response must be 500 without crashing the worker.

Exit code 2 for traversal or response-splitting findings; 1 for other failures.

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
| Dynamic segments `:int` | Negative values, zero, max i64 |
| Dynamic segments `:string` | Basic strings, hyphenated names |
| Dynamic segments `:uuid` | Valid UUIDs echoed back; invalid → 404 |
| Group-prefixed paths | `/api/v1/status`, `/api/v1/item/<id>` |
| `MwContinue` | `X-Route-MW: hit` present on `/mw/injected` |
| `MwBreak` | `/mw/blocked` → 403, handler never runs |
| `MwSkipNext` | Second middleware skipped, handler runs |
| Context storage | MW writes `uid=42`, handler echoes it |
| Async handler | `/async/sleep` returns `slept`; 20 concurrent hits |
| `ImJson` | `/json/im` body has all expected keys and values |
| `RmJson` | `/json/rm` body has all expected keys and values |
| JSON parsing | Valid bodies → 200; invalid/truncated/empty → 400 |
| Chained headers | `X-Chain-A/B/C` all present with exact values on `/chain` |
| Metrics | `/metrics` body has `network`, `process`, `log` keys |
| Templates | static, dynamic, conditional, loop, include, inherit |
| Template branch isolation | Wrong branches must not appear in output |
| Template inheritance | Child block overrides base; base title must not leak |
| Content-Type | All template routes return `text/html` |

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
| **dos-resistance** | Structural abuse: huge field counts, giant keys and values, deeply repeated separators. Must be bounded and refused, never hang |

Validators bound the **decoded** length, not the raw wire bytes, so a percent-
encoded payload cannot slip past a length check by being longer before decoding
than after.

Exit code 2 if header injection lands, 1 for other failures.

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
same as header values), path normalization still working with a query attached, and
buffer growth past `header_reserve_hint` (512 B) with a large query value.

Exit code 2 if a traversal-shaped value gets mangled, 1 for other failures.

---

### CORS

`CoreEngine::HandleCors` plus the generic `OPTIONS` `Allow:` fallback
(`CoreEngine::HandleGenericOptions`), tested against `app/wfx.toml`'s real `[CORS]`
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

Exit code 2 if an origin-matching bypass or credential leak lands, 1 for other
failures.

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

6 scenarios. Each is followed by a recovery wait and a full 52-route
correctness check. The master process is never targeted: only worker children.

| # | Scenario | What happens |
|---|----------|-------------|
| 1 | **Single worker kill × 3** | SIGKILL a random worker, 8 flood threads during restart, verify recovery |
| 2 | **Kills under sustained load** | 16 load threads for 30 s; one worker killed every 5 s |
| 3 | **Rapid-fire kills** | 1 kill every 2 s × 5: stresses restart/backoff machinery |
| 4 | **Dual-worker kill** | Both workers killed simultaneously; both must restart and serve correctly |
| 5 | **SIGSTOP → SIGCONT** | One worker paused 3 s while 8 threads hammer; unpaused; routes verified |
| 6 | **Kill under mixed load** | 75 half-open idle connections + 12 concurrent large-response threads; one worker killed mid-flight |

Recovery timeout is 15 s per scenario. Final `/metrics` is fetched and
crash + restart counts are reported.

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
| `/download` | Serves `public/<X-File>`: path-traversal target |
| `/violate/204body` | 204 with body: must return 500 |
| `/violate/conn` | Sets `Connection` header: engine-owned, must return 500 |
| `/violate/recommit` | Calls `Commit()` twice: must return 500 |
| `/metrics` | Live crash / restart / RSS / per-route counters as JSON |
| `/status/<code:uint>` | Returns the given status code: `metrics` phase's status-class workhorse |
| `/items/<id:uint>` | Dynamic `:uint` segment |
| `/items/signed/<id:int>` | Dynamic `:int` segment (negative values) |
| `/greet/<name:string>` | Dynamic `:string` segment |
| `/uuid/<id:uuid>` | Dynamic `:uuid` segment |
| `/api/v1/status` | Group-prefixed flat path |
| `/api/v1/item/<id:uint>` | Group-prefixed path with dynamic segment |
| `/mw/injected`, `/mw/blocked`, `/mw/skipnext`, `/ctx` | Middleware/context targets, see the FEATURES table above |
| `/async/sleep` | Async coroutine + `WFX::SleepFor(25ms)` |
| `/json/im` | `WFX::ImJson` streaming JSON |
| `/json/rm` | `WFX::RmJson` DOM-then-serialise JSON |
| `/parse-json` | `WFX::ParseJson` body parsing |
| `/chain` | Three chained `.Header()` calls |
| `/template/static` | Static template render |
| `/template/dynamic` | Template with runtime variable |
| `/template/cond/<n>` | Conditional branch isolation |
| `/template/loop` | Loop rendering |
| `/template/include` | Template include |
| `/template/inherit` | Template inheritance (child overrides block) |
