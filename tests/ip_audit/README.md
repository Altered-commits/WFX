# WFX IP Audit

Adversarial coverage of real-IP resolution (`WFX::Http::IpUtils::ResolveClientIp`) and
the two limiters it feeds, `ConnectionLimiter` and `RequestRateLimiter`, wired through
`CoreEngine::AllowRequest`. `app/wfx.toml` keeps `[IP]` permanently small (connection
cap 3, burst 5, refill 2/s, tracked-identity cap 64 - `BitmapPool`'s real minimum,
regardless of the configured value) instead of patching config mid-run, so every phase
but `dualstack` runs against the one server the whole time. Every phase's closing
liveness check uses a dedicated identity nothing else in the suite ever touches, so a
phase correctly rate-limiting its own attack traffic can never be misread as the server
having crashed.

---

## Requirements

- `wfx` on `PATH` (or pass `--wfx /path/to/wfx`)
- Python 3.8+, standard library only
- Linux with `/proc` (for worker PID discovery) and IPv6 loopback support (for `dualstack`)

---

## Quick start

```bash
cd tests/ip_audit

# All phases
python3 ip_audit.py

# Single phase
python3 ip_audit.py --phase eviction
python3 ip_audit.py --phase dualstack
python3 ip_audit.py --phase multiworker

# Different binary or port
python3 ip_audit.py --wfx /path/to/wfx --port 9090

# GitHub Actions
python3 ip_audit.py --ci
```

---

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | All phases passed |
| `1` | Correctness failure |
| `2` | **Security**: a trust-boundary spoof, reconnect, or dual-stack collapse actually bypassed a limiter |
| `3` | Server never came up |

---

## Phases

| Phase | What it proves |
|-------|-----------------|
| **trust_boundary** | `X-Forwarded-For` spoofing buys an untrusted peer nothing; recursive proxy-chain walking, CIDR-block trust, non-recursive fallback, and a hostile/malformed-header corpus that must never crash or 500. Also times a worst-case 300-hop chain against a 300-entry trusted-proxies list (`IsTrustedProxy` re-scans the whole list per hop, unmemoized, so this is genuinely `O(hops x proxies)`) to confirm it stays fast rather than stalling the worker. |
| **connection_cap** | `ConnectionLimiter` fill-refuse-release, per-identity isolation, spoofed-header evasion fails, and a real concurrent burst (not sequential) never lets more than the configured cap through. |
| **rate_limit** | `RequestRateLimiter`'s token bucket on one held keep-alive connection: burst succeeds, next request throttles, the same socket recovers once the bucket refills (a 429 no longer force-closes), and an explicit `Connection: close` still closes while throttled. |
| **eviction** | Fills the tracked-identity cap, confirms a new identity is denied while every slot is live, then frees one slot and confirms a previously-denied connection now succeeds on retry — the regression test for `ClientCtx`'s `rateLimiterAcquired` bit being decoupled from `ipAcquired`. |
| **dualstack** | See below — a real bug, not a bug demo. |
| **scale** | Drives 800 distinct identities through `ConnectionLimiter`'s uncapped `HashShard` and confirms both correctness and that the SipHash-keyed table doesn't slow down pathologically at real cardinality. |
| **multiworker** | See below — a real architectural limitation, not a bug. |

**dualstack** is the regression test for a real bug found while building this audit: on a
dual-stack listener (`wfx run --host ::`), an IPv4 peer arrives as an IPv4-mapped IPv6
address (`::ffff:a.b.c.d`), whose /64 mask is coarser than the mapped prefix's fixed 96
bits — every such peer used to collapse into one shared limiter identity, so one client
could exhaust the cap for every other IPv4 client on the box. This phase confirms two
distinct IPv4 peers now get independent caps, and that a native IPv6 connection to `::1`
(the only IPv6 loopback available without root) is capped correctly too.

**multiworker** documents a real architectural limitation, not a bug: each worker process
(`SO_REUSEPORT`) owns its own in-memory limiter state, and the kernel load-balances
connections across workers by a hash of the connection 4-tuple. One identity reconnecting
on a different source port can land on a different worker, so the effective cap becomes
`(configured cap) x (worker count)`, not the configured cap alone. This phase raises
`worker_processes` to 4 and confirms the multiplication is real and still bounded.
Operators who need an exact, non-multiplied cap should pin `worker_processes = 1`, same as
every other phase in this suite already does.

---

## Test app (`app/`)

A minimal WFX project exposing just `/health`. There's no per-scenario route to add:
real-IP resolution and both limiters run entirely inside `CoreEngine::AllowRequest`,
before a request ever reaches a handler, so the interesting surface is the engine
itself, not anything a route body could do.
