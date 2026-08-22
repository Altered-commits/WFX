# Testing

WFX's test suite lives in `tests/` as seven harnesses, called audits, rather than a conventional
unit test suite. Most of what actually needs proving here (a worker surviving a SIGKILL
mid-request, a malformed chunk size, a TLS client refusing a bad cert, a request whose body forces
the read buffer to relocate mid-parse) only shows up under real process and wire-level conditions,
not inside a unit test.

Six of the seven are adversarial: they send malformed or hostile input on purpose. The seventh,
`interop_audit`, is not: it proves the shipped clients work against real, spec-compliant servers
rather than surviving hostile ones, and has no security exit code.

---

## The seven audits

| Audit | Drives | Covers |
|-------|--------|--------|
| `base_audit` | WFX as an HTTP server | path traversal, CRLF/response-splitting, request smuggling, header/body abuse, every user-facing route, and 6 chaos scenarios that SIGKILL/SIGSTOP workers under load and verify recovery |
| `endpoint_audit` | the raw `WFX::Endpoint<>` primitive (outbound) | connection pooling and reservation, multiplexing several requests onto one connection, chunked streaming via a portal-style fetch, and graceful cancellation (`onAbort`), all against a hostile mock upstream it boots itself |
| `client_audit` | `WFX::HttpEndpoint`, `WFX::SmtpEndpoint` and `WFX::PostgresEndpoint` (outbound clients built on the primitive) | HTTP/1.1 framing, smuggling and desync defenses; the full SMTP `STARTTLS`/`AUTH` handshake and CR/LF/NUL injection defenses; and the Postgres `SSLRequest`/SCRAM handshake, extended-query wire protocol, statement cache and SQL-injection defenses, each protocol against its own hostile mock upstream the audit boots itself |
| `tls_audit` | the outbound client's TLS path | cert refusal (untrusted, hostname mismatch, expired, protocol downgrade), proven twice per persona: as a call error and as a mock-observed failed handshake, plus the framing/desync corpus replayed over TLS |
| `crypto_audit` | `wfx/utils/crypto.hpp` (hashing, HMAC, AEAD, KDFs, CSPRNG) | correctness against Python stdlib oracles (`hashlib`, `hmac`, `pbkdf2_hmac`, a hand-rolled RFC 5869 HKDF) where one exists, plus AEAD tamper/cross-algo rejection, a `WFX::HashStream` driven from inside a real `res.Stream()` callback, and a 64 MiB+1 body to exercise the AEAD size cap |
| `ip_audit` | real-IP resolution (`WFX::Http::IpUtils::ResolveClientIp`) and the `ConnectionLimiter`/`RequestRateLimiter` pair it feeds | `X-Forwarded-For` recursive-trust walking, connection/rate-limit caps and eviction, dual-stack (`::ffff:`-mapped) loopback handling, and hostile `X-Forwarded-For` corpora |
| `interop_audit` | `WFX::PostgresEndpoint`, `WFX::SmtpEndpoint` and `WFX::HttpEndpoint` against real, spec-compliant upstreams | real Postgres and two `smtp4dev` instances in Docker (one PLAIN, one LOGIN-only), and a second real WFX server (`upstream/`) as the HTTP leg |

Each audit is a standalone Python script (`base_audit.py`, `endpoint_audit.py`, `client_audit.py`,
`tls_audit.py`, `crypto_audit.py`, `ip_audit.py`, `interop_audit.py`) with its own small WFX
project under `app/` that exists purely to give the audit something to hit.

`interop_audit` is the odd one out: `client_audit` proves the same three clients survive a hostile
server, `interop_audit` proves they actually work against a real one, and it needs Docker (a real
Postgres container, two `smtp4dev` instances) to do it. See `tests/interop_audit/README.md` for
what each phase drives and its `docker-compose.yml` setup.

Every audit is a `common.Suite` subclass (see below) that declares an ordered `phases` dict of
`phase_<name>(ctx)` functions, selectable with `--phase <name>` or listed with `--list-phases`, so
adding or removing a phase never touches anything outside that one function and its registration.

!!! important
    These are read-only, non-network-touching from this repository's own tooling perspective:
    they compile and run the actual `wfx` binary and a project under it. There's no mocked-out
    version of WFX being tested here, the audits drive the real engine.

---

## Shared harness infrastructure (`tests/common/`)

All seven audits import this package for the boilerplate that doesn't decide what a test checks. It
is a proper Python package, not a single script, split by concern:

| Module | Provides |
|--------|----------|
| `common/suite.py` | `Suite`, the base class every audit subclasses; `Config`/`Context`/`Heartbeat`; the `run(suite_cls)` entry point; argument parsing, phase dispatch, worker-death detection, and the overall lifecycle |
| `common/report.py` | `Check`/`Phase`/`Report` - the bucketed pass/fail bookkeeping and the end-of-run summary table; the shared exit codes (see below) |
| `common/server.py` | `Server`, the WFX process under test: boot, health polling, revival detection, teardown |
| `common/net.py` | Hand-rolled raw HTTP over plain TCP or TLS (`send`, `request`, `status`, `body`, `headers`, `dechunk`, ...) - deliberately not a real HTTP client, since these suites send malformed framing and forged headers on purpose, which a real client would normalize or reject |
| `common/logs.py` | `LogFollower` (tails the running WFX project's worker/master/crash logs live) and `scan_crash_reports` (folds any on-disk ASan/UBSan report into a failing check) |
| `common/term.py` | Colored terminal output, progress ticks, and GitHub Actions `::group::`/`::error::`/`::warning::` commands under `--ci` |

A suite file itself contains only phase functions and a small `Suite` subclass declaring `name`,
`description`, and `phases`; see `tests/README.md` for the full "writing a suite" guide, hook list,
and style rules (name a check by the behavior it proves, use `p.secure(...)` only for actual
vulnerabilities, and so on) - that material isn't duplicated here since it would just drift out of
sync with this page.

---

## Running them

```bash
tests/run_audits.sh                  # the six non-interop audits, one after another
tests/run_audits.sh --audit base     # just one: base, endpoint, client, tls, crypto, ip, or interop
tests/run_audits.sh --ci             # forward --ci to whichever audits run
tests/run_audits.sh --audit tls -- --phase verify --wfx-logs all
                                      # anything after -- is passed through as-is
```

`interop_audit` needs Docker, so it is left out of the plain no-flag run (not every dev machine
has Docker running) and only reachable with `--audit interop`.

`run_audits.sh` never builds `wfx`. It expects a binary already on `PATH`, or at the repo root
where a normal build leaves one, the same way you'd build before running any single audit by hand.

---

## Exit codes

Identical across every suite (defined once in `common/report.py`), so CI can key on them without
caring which audit produced the result:

| Code | Meaning |
|------|---------|
| `0` | everything passed |
| `1` | a non-security check failed, or the worker was dead at the end |
| `2` | a **security** finding: a vector whose failure is a vulnerability |
| `3` | WFX never answered `/health`, so nothing was exercised |

---

## CI

Every audit shares one `--ci` flag (parsed by `Suite`'s common arguments): it disables ANSI colors
and emits GitHub Actions `::group::`/`::error::` annotations for failing checks so they surface in
the PR Checks UI, without changing any timeout or process behavior versus a local run.

`.github/workflows/audit_check.yml` never builds `wfx` itself. `compile_check.yml` uploads the
compiled binary as a 1-day-retention artifact; `audit_check.yml` downloads it and runs all seven
audits, including `interop`, as parallel matrix jobs, each calling `tests/run_audits.sh --audit
<name> --ci`. Docker and the `docker compose` plugin come preinstalled on the GitHub-hosted
runner, nothing extra to set up for the `interop` job. It's wired into `entry.yml` as the fourth
linear stage, gated on `compile_check.yml` passing first (in parallel with `tidy_check.yml`, the
fifth stage, both gated on the same compile step).

That downloaded binary is always built with `-DWFX_ENABLE_ASAN=ON` (see
[Build Macros](build_macros.md)) - it's CI-internal only, never distributed, so there's no downside
to it always being instrumented. The actual `ASAN_OPTIONS`/`UBSAN_OPTIONS` the `wfx` process under
test runs with come from `tests/common/logs.py`'s `launch_env()`, not from CI: every audit, local or
CI, launches `wfx` through it, and it sets `halt_on_error=1` on both plus a `log_path` pointing
sanitizer reports at `crash_logs` (see *Diagnosing a failure* in `tests/README.md`). `audit_check.yml`
also sets job-level `ASAN_OPTIONS`/`UBSAN_OPTIONS`, but `launch_env()` overwrites them for the `wfx`
subprocess either way, so the job-level values only matter if something else in the job reads them.

Leak detection runs for real, everywhere: nothing suppresses it process-wide. The one intentional,
permanent allocation in the engine, `GetLogger()`'s heap-allocated singleton (see
[Globals & Singletons](globals_and_singletons.md)), tells LeakSanitizer directly that it isn't a
leak via `__lsan_ignore_object`, instead of the test suite disabling leak detection to work around
it.

---

## Where things live

- `tests/common/`
    The shared package described above: `suite.py`, `report.py`, `server.py`, `net.py`, `logs.py`, `term.py`.

- `tests/run_audits.sh`
    Single entry point for running one or all audits, locally or from CI.

- `tests/base_audit/`, `tests/endpoint_audit/`, `tests/client_audit/`, `tests/tls_audit/`, `tests/crypto_audit/`, `tests/ip_audit/`
    One audit each: the harness script, its own `README.md`, its `app/` test project, and (for `endpoint_audit`/`client_audit`/`tls_audit`) one or more mock upstream scripts (`http_upstream.py` / `smtp_upstream.py` / `postgres_upstream.py` / `tls_upstream.py`) where the audit needs a hostile server on the other end.

- `tests/interop_audit/`
    The harness script, its own `README.md`, `docker-compose.yml` for the real Postgres/`smtp4dev` containers, `app/` (the client under test) and `upstream/` (a second real WFX server, the HTTP leg).

- `.github/workflows/audit_check.yml`
    Downloads the `wfx` binary artifact from `compile_check.yml` and runs all seven audits as a parallel CI matrix.
