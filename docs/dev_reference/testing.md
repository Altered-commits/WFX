# Testing

WFX's test suite lives in `tests/` as three adversarial harnesses, called audits, rather than a
conventional unit test suite. Most of what actually needs proving here (a worker surviving a
SIGKILL mid-request, a malformed chunk size, a TLS client refusing a bad cert) only shows up under
real process and wire-level conditions, not inside a unit test.

---

## The three audits

| Audit | Drives | Covers |
|-------|--------|--------|
| `base_audit` | WFX as an HTTP server | path traversal, CRLF/response-splitting, request smuggling, header/body abuse, every user-facing route, and 6 chaos scenarios that SIGKILL/SIGSTOP workers under load and verify recovery |
| `endpoint_audit` | `WFX::HttpEndpoint` (outbound client) | framing, chunked/close-delimited bodies, connection pooling, coalescing, multiplexing, and lifecycle behavior, all against a hostile mock upstream it boots itself |
| `tls_audit` | the outbound client's TLS path | cert refusal (untrusted, hostname mismatch, expired, protocol downgrade), proven twice per persona: as a call error and as a mock-observed failed handshake, plus the framing/desync corpus replayed over TLS |

Each audit is a standalone Python script (`base_audit.py`, `endpoint_audit.py`, `tls_audit.py`)
with its own small WFX project under `app/` that exists purely to give the audit something to hit.
Each phase within an audit is one function, registered in a `PHASES` list/dict, selectable with
`--phase <name>` or listed with `--list-phases`, so adding or removing a phase never touches
anything outside that one function and its registration.

!!! important
    These are read-only, non-network-touching from this repository's own tooling perspective:
    they compile and run the actual `wfx` binary and a project under it. There's no mocked-out
    version of WFX being tested here, the audits drive the real engine.

---

## Shared harness infrastructure (`tests/_audit_common.py`)

All three audits import this module for the boilerplate that doesn't decide what a test checks:

- colored terminal output, and GitHub Actions `::group::`/`::error::`/`::warning::` commands under `--ci`
- `LogFollower`, a thread that tails the running WFX project's worker/master/crash logs live, so a boot-time crash prints as it happens instead of vanishing on worker revival
- a raw stdlib-socket HTTP client (`raw_send`, `build_request`, `response_status`, `response_body`)
- `Results` / `check()` / `format_report()`, the bucketed pass/fail bookkeeping `endpoint_audit` and `tls_audit` both use to build their report table
- `add_common_args()`, the shared argparse scaffolding (`--host`, `--port`, `--wfx`, `--app-dir`, `--phase`, `--ci`, `--wfx-logs`, ...)

`base_audit` only borrows the colors, GitHub Actions helpers, and the raw HTTP client. Its own
phase results are timed findings-lists rather than the bucket shape the other two use, and its
`raw_send` defaults differ on purpose, so it keeps its own copy instead of forcing a shared one.

---

## Running them

```bash
tests/run_audits.sh                  # all three, one after another
tests/run_audits.sh --audit base     # just one: base, endpoint, or tls
tests/run_audits.sh --ci             # forward --ci to whichever audits run
tests/run_audits.sh --audit tls -- --phase verify --wfx-logs all
                                      # anything after -- is passed through as-is
```

`run_audits.sh` never builds `wfx`. It expects a binary already on `PATH`, or at the repo root
where a normal build leaves one, the same way you'd build before running any single audit by hand.

---

## CI

Every audit shares one `--ci` flag (wired through `add_common_args`): it disables ANSI colors and
emits GitHub Actions `::group::`/`::error::` annotations for failing checks so they surface in the
PR Checks UI, without changing any timeout or process behavior versus a local run.

`.github/workflows/audit_check.yml` never builds `wfx` itself. `compile_check.yml`'s gcc leg uploads
the compiled binary as a 1-day-retention artifact; `audit_check.yml` downloads it and runs the three
audits as parallel matrix jobs, each calling `tests/run_audits.sh --audit <name> --ci`. It's wired
into `entry.yml` as the fourth linear stage, gated on `compile_check.yml` passing first.

---

## Where things live

- `tests/_audit_common.py`  
    Shared colors, GitHub Actions helpers, log follower, raw HTTP client, and check/report bookkeeping.

- `tests/run_audits.sh`  
    Single entry point for running one or all audits, locally or from CI.

- `tests/base_audit/`, `tests/endpoint_audit/`, `tests/tls_audit/`  
    One audit each: the harness script, its own `README.md`, its `app/` test project, and a mock upstream script (`upstream.py` / `tls_upstream.py`) where the audit needs a hostile server on the other end.

- `.github/workflows/audit_check.yml`  
    Downloads the `wfx` binary artifact from `compile_check.yml` and runs the three audits as a parallel CI matrix.
