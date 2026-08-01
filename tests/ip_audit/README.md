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

### trust_boundary

`X-Forwarded-For` trust-boundary spoofing (an untrusted peer's forged header buys it
nothing), distinct-identity isolation, reconnect-resistance (the rate limiter must not
reset just because the client opens a new connection per request), recursive chain
walking (including where an untrusted middle hop blocks the walk), CIDR-block trust
(a source inside a configured block gets its header honored, a source just outside it
does not), non-recursive mode (a multi-value chain fails to parse as one address and
falls back to the peer IP), and a corpus of hostile/malformed header values that must
never crash or 500. Also regression-tests that an IPv4-mapped IPv6 literal inside the
header text (`::ffff:a.b.c.d`) resolves to the exact same identity as its plain IPv4 form.

Also drives the recursive walk's actual worst case: `IsTrustedProxy` re-scans the whole
`trusted_proxies` list, unmemoized, for every hop in the chain, so a long chain against a
long trusted-proxies list is `O(hops x proxies)` CIDR parses on the single-threaded event
loop every other client is also waiting on. A 300-hop chain against a 300-entry list,
constructed so nothing short-circuits early, is timed to confirm it stays fast rather than
stalling the worker.

### connection_cap

`ConnectionLimiter`: fill-refuse-release for one identity, distinct identities (varying
one of the first three octets, since `NormalizeIp` masks IPv4 to /24 - varying only the
last octet collapses every "distinct" address into the same key), and confirms an
untrusted peer spoofing a different `X-Forwarded-For` per connection still can't dodge
its own real cap. Also fires a burst of simultaneous connection attempts (not sequential)
at the same identity, confirming the accept loop's single-pass drain never lets more than
the configured cap through even under a real concurrent hit.

### rate_limit

`RequestRateLimiter`'s token bucket, driven entirely on one held keep-alive connection:
a full burst succeeds, the next request throttles, and - since a 429 no longer forces
the connection closed - the same socket recovers once the bucket refills, with no
reconnect needed. Also confirms a request that explicitly asks for `Connection: close`
still actually closes even while being throttled.

### eviction

Fills the tracked-identity cap with distinct, live (still-connected) identities, then
confirms a brand new identity is denied outright while every tracked slot is live (no
eviction candidate exists). Frees one slot, then retries on the same denied connection
and confirms it now succeeds - the direct regression test for `ClientCtx`'s
`rateLimiterAcquired` bit being decoupled from `ipAcquired`, so a connection that failed
under momentary cap pressure keeps retrying instead of staying stuck for its lifetime.
Also confirms an evicted identity reconnects cleanly with a fresh bucket rather than
erroring.

### dualstack

Regression test for a real bug found while building this audit: on a dual-stack listener
(`wfx run --host ::`), an ordinary IPv4 peer arrives at `accept()` as an IPv4-mapped IPv6
address (`::ffff:a.b.c.d`). `NormalizeIp`'s /64 IPv6 mask is coarser than the mapped
prefix's fixed 96 bits, so every such peer used to collapse into one shared limiter
identity - one client could exhaust the cap for every other IPv4 client on the box. This
phase restarts the server bound to `::`, drives two distinct loopback IPv4 peers, and
confirms they get independent caps. Also drives a genuine (non-mapped) native IPv6
connection to `::1` and confirms its own cap is enforced correctly too - `::1` is the only
IPv6 loopback address available without root, so this can prove that one v6 identity works
end to end, not that two distinct v6 peers are told apart.

### scale

`ConnectionLimiter`'s `HashShard` has no eviction or cap at all (unlike
`RequestRateLimiter`'s `BitmapPool`-bound pool) - it self-bounds only via
erase-on-refCount-0, so its real size tracks however many identities are genuinely
concurrent. Drives 800 distinct identities through it and confirms both correctness (every
one succeeds) and that the SipHash-keyed table with its grow-only resize doesn't slow down
pathologically at real cardinality, not just the handful of addresses every other phase
uses.

### multiworker

Documents and demonstrates a real architectural limitation rather than a bug: each worker
process (`SO_REUSEPORT`, confirmed in `epoll_connection.cpp`) owns its own in-memory
`ConnectionLimiter`/`RequestRateLimiter`. The kernel load-balances connections across
worker listening sockets by a hash of the connection 4-tuple, so one identity reconnecting
on different source ports lands on different workers - meaning the effective cap for that
identity is `(configured cap) x (worker count)`, not the configured cap alone. This phase
temporarily raises `worker_processes` to 4 and confirms the multiplication is real and
still bounded (never more than `cap x workers`), rather than just asserting it from
reading the code. Operators who need an exact, non-multiplied cap should pin
`worker_processes = 1`, same as every other phase in this suite already does.

---

## Test app (`app/`)

A minimal WFX project exposing just `/health`. There's no per-scenario route to add:
real-IP resolution and both limiters run entirely inside `CoreEngine::AllowRequest`,
before a request ever reaches a handler, so the interesting surface is the engine
itself, not anything a route body could do.
