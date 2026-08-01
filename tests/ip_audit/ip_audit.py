#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX IP audit
#
# Adversarial coverage of real-IP resolution (WFX::Http::IpUtils::ResolveClientIp,
# http/limits/ip_utils.cpp) and the two limiters it feeds (ConnectionLimiter,
# RequestRateLimiter, wired through CoreEngine::AllowRequest in engine/core_engine.cpp).
# app/wfx.toml keeps [IP] permanently small (cap=3, burst=5, rate=2/s, tracked-identity
# cap=64, BitmapPool's real minimum) instead of patching config mid-run, so every phase-
# -but dualstack runs against the same server the whole time.
#
# Phases:
#   trust_boundary   XFF trust-boundary spoofing, CIDR-block trust, recursive/non-recursive
#                    chain walking, an adversarial worst-case recursive-chain timing check,
#                    hostile/malformed header parsing, IPv4-mapped-IPv6 literal
#   connection_cap   ConnectionLimiter: fill/refuse/release, distinct identities, spoofing
#                    cannot dodge the peer's own cap, a concurrent burst still holds exactly
#                    at the cap (no accept-race lets it over-count)
#   rate_limit       RequestRateLimiter: burst/refill on one held connection, reconnect
#                    cannot reset a bucket, 429 respects keep-alive per the Connection header
#   eviction         LRU eviction under real capacity pressure: live entries protected,
#                    a saturated-cap connection recovers once capacity frees up
#   dualstack        IPv4-mapped-IPv6 peer collapse: distinct v4 clients over a dual-stack
#                    listener must not share one limiter identity; a native (non-mapped)
#                    IPv6 peer is enforced correctly too
#   scale            ConnectionLimiter's HashShard under real cardinality: hundreds of
#                    distinct identities, all correct, no pathological slowdown
#   multiworker      Documents and demonstrates a real architectural limitation: each
#                    worker process has independent limiter state, so the effective cap
#                    is (configured cap) x (worker count), not the configured cap alone
#
# Usage:
#   python3 ip_audit.py                    # all phases
#   python3 ip_audit.py --phase eviction
#   python3 ip_audit.py --list-phases

import concurrent.futures
import os
import re
import socket
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import common
from common import net, term

# Config baked into app/wfx.toml's [IP] section, kept here so the numbers in this file-
# -read the same as the ones a phase actually drives against
CAP        = 3   # max_connections_per_ip
BURST      = 5   # max_request_burst_per_ip
RATE       = 2   # max_requests_per_ip_per_sec
TRACK_CAP  = 64  # max_tracked_identities, rounded up to 64 by BitmapPool regardless of the-
                 # -configured value, so 64 is the real cap a phase has to actually fill

TRUSTED    = "127.0.0.1"  # matches trusted_proxies in wfx.toml
# UNTRUSTED/UNTRUSTED2 deliberately differ from TRUSTED (and each other) in more than the-
# -last octet: NormalizeIp's /24 mask would otherwise collapse "127.0.0.1" and "127.0.0.2"-
# -into the identical identity, and the suite's own startup wait_ready() probe (which hits-
# -TRUSTED's bare identity with no XFF, before any phase runs at all) would then silently-
# -eat a token from whatever bucket UNTRUSTED also happens to share
UNTRUSTED  = "127.0.2.2"  # loopback, but deliberately NOT in trusted_proxies
UNTRUSTED2 = "127.0.1.2"  # a SECOND, distinct untrusted identity: connection_cap needs its own,-
                          # -since trust_boundary's G1 already drains UNTRUSTED's own bucket

# Dedicated identity for liveness probes only: nothing else in this suite ever sends from-
# -this address, so a phase's own attack traffic can never make _alive() misreport "dead"-
# -just because it correctly rate-limited some OTHER identity that happens to share a bucket-
# -with a generic health check (this is exactly what happened to common.health() during-
# -development: it uses the bare peer identity with no way to route around a drained bucket)
_ALIVE_PROBE = "127.9.9.9"

_REFILL_WAIT = 1.0 / RATE + 0.3  # time for >=1 token to refill, plus margin

# Transport
def _build(method, path, headers=None, close=True):
    lines = ["%s %s HTTP/1.1" % (method, path), "Host: x"]
    if close:
        lines.append("Connection: close")
    else:
        lines.append("Connection: keep-alive")
    for k, v in (headers or {}).items():
        lines.append("%s: %s" % (k, v))
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1")

def _read_http_response(sock, buf, rtimeout=4.0):
    """Reads exactly one CL-framed response off 'sock'. Returns (status, leftover-buf), or-
    -(None, buf) on a closed/dead socket - the caller decides what that means"""
    sock.settimeout(rtimeout)

    while b"\r\n\r\n" not in buf:
        try:
            d = sock.recv(65536)
        except OSError:
            return None, buf
        if not d:
            return None, buf
        buf += d

    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    try:
        status = int(lines[0].split(b" ")[1])
    except (IndexError, ValueError):
        return None, rest

    clen = 0
    for line in lines[1:]:
        if line.lower().startswith(b"content-length:"):
            try:
                clen = int(line.split(b":", 1)[1].strip())
            except ValueError:
                clen = 0
            break

    while len(rest) < clen:
        try:
            d = sock.recv(65536)
        except OSError:
            break
        if not d:
            break
        rest += d

    return status, rest[clen:]

def _send_from(host, port, source_ip, method, path, headers=None, rtimeout=4.0):
    """One-shot Connection: close request from a bound source address - the only way to-
    -make WFX see a different peer IP without root (any 127.0.0.0/8 address is a distinct,-
    -valid loopback source on Linux). Returns the status, or 'CONN_ERR' on a failed connect"""
    sock = None
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind((source_ip, 0))
        sock.settimeout(rtimeout)
        sock.connect((host, port))
        sock.sendall(_build(method, path, headers, close=True))
        status, _ = _read_http_response(sock, b"", rtimeout)
        return status if status is not None else "CONN_ERR"
    except OSError:
        return "CONN_ERR"
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass

def _open_held(host, port, source_ip, path="/health", headers=None, rtimeout=4.0):
    """Opens a socket bound to 'source_ip', sends one Connection: keep-alive request, reads-
    -exactly one response, and returns (status, sock, buf) leaving the socket OPEN so a later-
    -request can be sent on it via _send_on(). (None, None, b'') on a failed connect"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind((source_ip, 0))
        sock.settimeout(rtimeout)
        sock.connect((host, port))
        sock.sendall(_build("GET", path, headers, close=False))
        status, buf = _read_http_response(sock, b"", rtimeout)
        return status, sock, buf
    except OSError:
        try:
            sock.close()
        except OSError:
            pass
        return None, None, b""

def _send_on(sock, buf, path="/health", headers=None, close=False, rtimeout=4.0):
    """Sends one more request over an already-open socket from _open_held(). Returns-
    -(status, leftover-buf), or (None, buf) if the socket was already closed server-side"""
    try:
        sock.sendall(_build("GET", path, headers, close=close))
    except OSError:
        return None, buf
    return _read_http_response(sock, buf, rtimeout)

def _open_held_v6(port, path="/health", headers=None, rtimeout=4.0):
    """Like _open_held(), but a genuine (non-mapped) native IPv6 loopback connection. ::1 is
    the only IPv6 address available without root, so this proves ::1 itself is enforced
    correctly through NormalizeIp's v6 branch - not that two distinct v6 peers are, which
    would need a second address loopback alone can't provide"""
    sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    try:
        sock.settimeout(rtimeout)
        sock.connect(("::1", port))
        sock.sendall(_build("GET", path, headers, close=False))
        status, buf = _read_http_response(sock, b"", rtimeout)
        return status, sock, buf
    except OSError:
        try:
            sock.close()
        except OSError:
            pass
        return None, None, b""

def _close_all(socks):
    for s in socks:
        if s is not None:
            try:
                s.close()
            except OSError:
                pass

def _drain_burst(host, port, source_ip, path="/health", headers=None, n=BURST + 1):
    """Sends 'n' fresh Connection: close requests (one identity, reconnecting every time).
    Returns (index of the first non-200, last status, every status seen so far) - the full
    list is for failure detail only, so a wrong count is diagnosable from the log alone
    instead of needing a re-run. Regression shape for the reconnect-resets-the-bucket bug:
    a correct limiter still hits the cap at BURST"""
    hit, st, seen = None, None, []
    for i in range(n):
        st = _send_from(host, port, source_ip, "GET", path, headers)
        seen.append(st)
        if st != 200:
            hit = i
            break
    return hit, st, seen

def _alive(host, port, rtimeout=4.0):
    """Is the server structurally alive and answering well-formed HTTP, regardless of its own
    rate/connection-limiting decisions? A 429/503 is still proof of life. Uses _ALIVE_PROBE so
    a phase's own attack traffic against some OTHER identity can never cause a false reading"""
    return isinstance(_send_from(host, port, _ALIVE_PROBE, "GET", "/health"), int)

# TOML patching, only phases that need a config variant other than app/wfx.toml's-
# -permanent baseline use this (currently just the non-recursive sub-case below)
def _patch_toml(path, pattern, replacement):
    with open(path) as f:
        original = f.read()
    patched = re.sub(pattern, replacement, original, count=1)
    with open(path, "w") as f:
        f.write(patched)
    return original

def _restore_toml(path, original):
    with open(path, "w") as f:
        f.write(original)

def _restart(ctx):
    ctx.server.stop(confirm_exit=True)
    ctx.server.start()
    return ctx.server.wait_ready() is not None

def _restart_with_flags(ctx, flags):
    ctx.server.flags = list(flags)
    return _restart(ctx)

# Phase: TRUST_BOUNDARY
def phase_trust_boundary(ctx):
    cfg = ctx.cfg
    host, port = cfg.host, cfg.port
    p = ctx.phase("trust_boundary")

    # G1: untrusted peer - rotating a spoofed XFF buys nothing, the header is never honored-
    # -for a peer outside trusted_proxies, so every attempt is billed to 127.0.0.2 itself
    hit, st, seen = None, None, []
    for i in range(BURST + 1):
        st = _send_from(host, port, UNTRUSTED, "GET", "/health", {"X-Forwarded-For": "10.10.0.%d" % i})
        seen.append(st)
        if st != 200:
            hit = i
            break
    p.secure("untrusted peer: rotating a spoofed XFF cannot outrun its own rate limit",
             hit == BURST and st == 429, "expected 429 at #%d, got %r at #%r, full sequence=%r"
             % (BURST, st, hit, seen))

    # G2: trusted peer - distinct resolved identities do not share a bucket
    hit, st, seen = _drain_burst(host, port, TRUSTED, headers={"X-Forwarded-For": "10.10.1.1"})
    p.check("trusted peer: identity A's own burst drains to 429",
            hit == BURST and st == 429, "got %r at #%r, sequence=%r" % (st, hit, seen))
    st2 = _send_from(host, port, TRUSTED, "GET", "/health", {"X-Forwarded-For": "10.10.2.1"})
    p.secure("trusted peer: identity B is untouched by identity A's drained bucket",
             st2 == 200, "status=%r, expected 200 (distinct /24 = distinct identity)" % st2)

    # G3: trusted peer - reconnecting every request cannot reset one identity's bucket-
    # -(direct regression test: this used to erase-and-reseed the bucket on every reconnect)
    hit, st, seen = _drain_burst(host, port, TRUSTED, headers={"X-Forwarded-For": "10.10.3.1"})
    p.secure("trusted peer: reconnecting every request cannot reset one identity's bucket",
             hit == BURST and st == 429, "expected 429 at #%d, got %r at #%r, sequence=%r"
             % (BURST, st, hit, seen))

    # G4: the bucket actually refills, not a permanent ban
    time.sleep(_REFILL_WAIT)
    st = _send_from(host, port, TRUSTED, "GET", "/health", {"X-Forwarded-For": "10.10.3.1"})
    p.check("bucket refills after waiting", st == 200, "status=%r after waiting for refill" % st)

    # G5: resolution only happens on a connection's first request - rotating XFF mid-stream-
    # -on the SAME connection cannot escape the identity that request resolved to
    st0, sock, buf = _open_held(host, port, TRUSTED, headers={"X-Forwarded-For": "10.10.5.1"})
    hit, st = None, st0
    if sock is not None:
        for i in range(1, BURST + 1):
            st, buf = _send_on(sock, buf, headers={"X-Forwarded-For": "10.10.5.%d" % (100 + i)})
            if st != 200:
                hit = i
                break
        sock.close()
    p.secure("rotating X-Forwarded-For mid-connection cannot escape the cached identity",
             hit == BURST and st == 429, "expected 429 at request #%d, got %r at #%r" % (BURST, st, hit))
    st_fresh = _send_from(host, port, TRUSTED, "GET", "/health", {"X-Forwarded-For": "10.10.5.1"})
    p.secure("the identity actually billed was the first XFF, not the rotated ones",
             st_fresh == 429, "status=%r, expected 429 (10.10.5.1's bucket should be spent)" % st_fresh)

    toml_path = os.path.join(cfg.app_dir, "wfx.toml")

    # G6: recursive chain walking - walks right-to-left past trusted hops, stops at the-
    # -first untrusted one and uses that as the real client
    hit, st, seen = _drain_burst(host, port, TRUSTED, headers={"X-Forwarded-For": "10.10.6.1, 127.0.0.1"})
    p.check("recursive: one trusted hop, real client resolved past it",
            hit == BURST and st == 429, "got %r at #%r, sequence=%r" % (st, hit, seen))

    # The real client (10.71.1.1) and the blocking untrusted hop (10.72.2.2) MUST differ in-
    # -more than their last octet, or NormalizeIp's /24 mask collapses them into one identity-
    # -and this test can't tell "resolved past the block" from "just the same bucket"
    hit, st, seen = _drain_burst(host, port, TRUSTED,
                                 headers={"X-Forwarded-For": "10.71.1.1, 10.72.2.2, 127.0.0.1"})
    p.check("recursive: walk stops at the first untrusted hop from the right",
            hit == BURST and st == 429, "got %r at #%r, sequence=%r" % (st, hit, seen))
    st_untouched = _send_from(host, port, TRUSTED, "GET", "/health", {"X-Forwarded-For": "10.71.1.1"})
    p.secure("recursive: the hop beyond the untrusted stop point is never reached or billed",
             st_untouched == 200, "status=%r, expected 200 (10.71.1.1 should be fresh)" % st_untouched)

    # G6b: trusted_proxies matches a whole CIDR block, not just the one /32 baked into-
    # -wfx.toml - a source inside the block gets the header honored, a source just outside-
    # -it (still loopback, still "close" numerically) does not
    original_cidr = _patch_toml(toml_path, r'(?m)^(\s*trusted_proxies\s*=\s*)\[.*\]',
                                r'\g<1>["127.0.10.0/24"]')
    try:
        if not _restart(ctx):
            p.failed("TRUST_CIDR_BOOT", "server never came back with a /24 trusted_proxies block")
        else:
            hit, st, seen = _drain_burst(host, port, "127.0.10.5", headers={"X-Forwarded-For": "10.90.1.1"})
            p.secure("CIDR block: a source inside it has its XFF honored",
                     hit == BURST and st == 429, "got %r at #%r, sequence=%r" % (st, hit, seen))

            st_outside = _send_from(host, port, "127.0.11.5", "GET", "/health",
                                    {"X-Forwarded-For": "10.90.1.1"})
            p.secure("CIDR block: a source just outside it does not get its XFF honored",
                     st_outside == 200,
                     "status=%r, expected 200 (127.0.11.5 is outside 127.0.10.0/24, spoofing 10.90.1.1 "
                     "which is already drained should not have worked)" % st_outside)
    finally:
        _restore_toml(toml_path, original_cidr)
        if not _restart(ctx):
            p.failed("TRUST_CIDR_RESTORE_BOOT", "server never came back after restoring trusted_proxies")

    # G6c: regression for a whitespace-trim bug found via manual code review, not by any test-
    # -until now: when the recursive walk exits because the ENTIRE chain, including its-
    # -leftmost hop, is trusted, the old code left 'candidate' holding the untrimmed original-
    # -text instead of the already-trimmed 'hop' - a leading/trailing space on that leftmost-
    # -token then made the final ParseIpAddress fail and silently fall back to the peer.
    # -Whitespace around the WHOLE header value is already stripped by the HTTP parser itself-
    # -(http_parser.cpp's own Trim() on every header value) before this code ever sees it, so-
    # -the trigger has to be INTERNAL whitespace next to a comma inside a multi-hop chain -
    # -a bare single value wrapped in spaces never reaches ResolveClientIp with any left at all
    original_ws = _patch_toml(toml_path, r'(?m)^(\s*trusted_proxies\s*=\s*)\[.*\]',
                              r'\g<1>["127.0.0.1/32", "10.10.12.5/32"]')
    try:
        if not _restart(ctx):
            p.failed("TRUST_WS_BOOT", "server never came back with a second trusted_proxies entry")
        else:
            # Drain the peer's own bare identity first, so a silent fallback-to-peer is-
            # -distinguishable from a correct resolution to 10.10.12.5 (a fresh identity)
            hit0, st0, seen0 = _drain_burst(host, port, TRUSTED, headers=None, n=BURST + 2)
            p.check("whitespace regression: peer's own bare identity can be drained first",
                    hit0 is not None and st0 == 429, "sequence=%r" % seen0)

            # Two hops, both trusted: a space sits right before the comma, on the LEFTMOST-
            # -hop specifically - candidate.substr(0, comma) preserves that trailing space,-
            # -and it's still there when the loop exits via the "ran out of commas" path
            st_ws = _send_from(host, port, TRUSTED, "GET", "/health",
                               {"X-Forwarded-For": "10.10.12.5 , 127.0.0.1"})
            p.secure("whitespace next to a comma on an all-trusted chain's leftmost hop still "
                     "resolves to that hop, not a silent fallback to the drained peer",
                     st_ws == 200, "status=%r, expected 200 (10.10.12.5 should be fresh, not "
                     "the already-drained peer identity)" % st_ws)
    finally:
        _restore_toml(toml_path, original_ws)
        if not _restart(ctx):
            p.failed("TRUST_WS_RESTORE_BOOT", "server never came back after restoring trusted_proxies")

    # G7: non-recursive mode takes the header value as-is, no chain walking. A multi-value-
    # -chain fails to parse as one address and falls back to the peer IP - fails safe
    original = _patch_toml(toml_path, r'(?m)^(\s*real_ip_recursive\s*=\s*)true', r'\g<1>false')
    try:
        if not _restart(ctx):
            p.failed("TRUST_NONRECURSIVE_BOOT", "server never came back with real_ip_recursive=false")
        else:
            hit, st, seen = _drain_burst(host, port, TRUSTED, headers={"X-Forwarded-For": "10.10.8.1"})
            p.check("non-recursive: a bare single value still resolves directly",
                    hit == BURST and st == 429, "got %r at #%r, sequence=%r" % (st, hit, seen))

            # Not an exact-count check like the others: _restart() above just ran wait_ready(),
            # -which itself probes TRUSTED's bare identity (no XFF) once, so this bucket can-
            # -start already one token short. The property under test is just "the bare peer-
            # -identity is rate-limited at all in non-recursive mode", so a generous margin-
            # -over BURST is used instead of asserting the exact index
            hit2, st2, seen2 = _drain_burst(host, port, TRUSTED, headers=None, n=BURST + 2)
            p.check("non-recursive: peer's own bare identity can be drained",
                    hit2 is not None and st2 == 429,
                    "never hit 429 within %d tries, sequence=%r" % (BURST + 2, seen2))
            st3 = _send_from(host, port, TRUSTED, "GET", "/health",
                             {"X-Forwarded-For": "10.10.8.2, 127.0.0.1"})
            p.secure("non-recursive: a multi-value chain fails to parse and falls back to the peer",
                     st3 == 429, "status=%r, expected 429 (falls back to the already-drained peer)" % st3)
    finally:
        _restore_toml(toml_path, original)
        if not _restart(ctx):
            p.failed("TRUST_RESTORE_BOOT", "server never came back after restoring real_ip_recursive")

    # G8: malformed / hostile X-Forwarded-For values must never crash or 500-fault
    hostile = [
        "",                                   # empty
        "not-an-ip-at-all",                    # garbage
        "'; DROP TABLE users; --",              # injection-flavoured garbage
        "999.999.999.999",                      # out-of-range octets
        "A" * 2000,                             # oversized single value
        ",".join(["127.0.0.1"] * 700),          # long chain, still under max_header_size
        "127.0.0.1, 127.0.0.1, 127.0.0.1",      # whole chain is the trusted proxy itself
        "  10.10.9.9  ",                        # surrounding whitespace
        "::1",                                  # bare IPv6 literal
        "fe80::1%eth0",                         # zone-id suffix, not accepted by inet_pton
        "0177.0.0.1",                           # octal-looking octet
        "2130706433",                           # decimal single-integer form
    ]
    with term.progress("trust_boundary", "hostile-xff", len(hostile)) as pr:
        for val in hostile:
            st = _send_from(host, port, TRUSTED, "GET", "/health", {"X-Forwarded-For": val})
            ok = st != 500 and st != "CONN_ERR" and st is not None
            (pr.ok if ok else pr.bad)()
            if not ok:
                p.secure("hostile XFF %r does not crash or 500-fault the server" % (val[:40],),
                         False, "status=%r" % st)
    p.check("hostile XFF corpus: no crash across %d vectors" % len(hostile), True)

    dup_raw = (b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
               b"X-Forwarded-For: 10.10.9.1\r\nX-Forwarded-For: 10.10.9.2\r\n\r\n")
    raw = net.send(host, port, dup_raw, rtimeout=4.0)
    st = net.status(raw)
    p.secure("duplicate X-Forwarded-For headers do not crash or 500-fault the server",
             st != 500 and st is not None, "status=%r" % st)

    # Regression: an IPv4-mapped IPv6 literal INSIDE the header text must resolve identically-
    # -to its plain IPv4 form (ip_utils.cpp's ParseIpAddress collapses ::ffff:a.b.c.d to AF_INET)
    hit, st, seen = _drain_burst(host, port, TRUSTED, headers={"X-Forwarded-For": "::ffff:10.10.10.5"})
    p.check("v4-mapped XFF literal: drains its own burst like a normal address",
            hit == BURST and st == 429, "got %r at #%r, sequence=%r" % (st, hit, seen))
    st_plain = _send_from(host, port, TRUSTED, "GET", "/health", {"X-Forwarded-For": "10.10.10.5"})
    p.secure("v4-mapped XFF literal collapses to the same identity as its plain IPv4 form",
             st_plain == 429, "status=%r, expected 429 (::ffff:10.10.10.5 and 10.10.10.5 must be one identity)" % st_plain)

    # G9: adversarial cost of the recursive walk itself. IsTrustedProxy re-scans the WHOLE-
    # -trusted_proxies list, unmemoized, for every single hop in the chain - so a long chain-
    # -against a long trusted_proxies list is O(hops * proxies) CIDR parses per request, on the-
    # -single-threaded event loop every other client is also waiting on. Worst case: every hop-
    # -matches only the LAST entry in trusted_proxies, so nothing short-circuits early
    decoys = ['"127.200.%d.1/32"' % i for i in range(1, 300)]
    matching_hop = "127.5.5.5"
    proxies_toml = "[" + ", ".join(decoys + ['"%s/32"' % matching_hop]) + "]"
    # Real client goes LEFTMOST (walked last): the walk starts at the RIGHTMOST hop and moves-
    # -left, so 300 trusted hops must sit to the right of it, or the walk stops after one step
    chain = ", ".join(["10.10.11.1"] + [matching_hop] * 300)

    original_scale = _patch_toml(toml_path, r'(?m)^(\s*trusted_proxies\s*=\s*)\[.*\]',
                                 lambda m: m.group(1) + proxies_toml)
    try:
        if not _restart(ctx):
            p.failed("TRUST_CHAIN_COST_BOOT", "server never came back with a 300-entry trusted_proxies list")
        else:
            start = time.time()
            st_cost = _send_from(host, port, matching_hop, "GET", "/health", {"X-Forwarded-For": chain})
            elapsed = time.time() - start
            # Generous bound: a well-behaved O(hops*proxies) walk finishes in low milliseconds:
            # -a multi-second stall here would mean every other client on this worker is also-
            # -blocked for that long, which is the actual DoS surface being checked for
            p.secure("worst-case recursive chain (300 hops x 300 trusted_proxies) stays fast",
                     elapsed < 2.0, "took %.3fs, status=%r (expected well under 2s)" % (elapsed, st_cost))
    finally:
        _restore_toml(toml_path, original_scale)
        if not _restart(ctx):
            p.failed("TRUST_CHAIN_COST_RESTORE_BOOT", "server never came back after restoring trusted_proxies")

    if not _alive(host, port):
        p.failed("TRUST_SERVER_DEAD", "did not answer the dedicated liveness probe")

# Phase: CONNECTION_CAP
def phase_connection_cap(ctx):
    cfg = ctx.cfg
    host, port = cfg.host, cfg.port
    p = ctx.phase("connection_cap")

    # G1: fill / refuse / release for one identity
    socks = []
    try:
        setup_ok = True
        for _ in range(CAP):
            st, s, _ = _open_held(host, port, TRUSTED, headers={"X-Forwarded-For": "10.20.1.1"})
            socks.append(s)
            if st != 200:
                setup_ok = False
        p.check("connection cap: the first %d connections for one identity all fit" % CAP, setup_ok)

        st_over, s_over, _ = _open_held(host, port, TRUSTED, headers={"X-Forwarded-For": "10.20.1.1"})
        socks.append(s_over)
        p.secure("connection cap: the (cap+1)th connection for one identity is refused",
                 st_over == 503, "status=%r, expected 503" % st_over)

        if socks and socks[0] is not None:
            socks[0].close()
            socks[0] = None
            time.sleep(0.1)
            st_after, s_after, _ = _open_held(host, port, TRUSTED, headers={"X-Forwarded-For": "10.20.1.1"})
            socks.append(s_after)
            p.check("connection cap: closing a held connection frees its slot for reuse",
                    st_after == 200, "status=%r, expected 200 after freeing a slot" % st_after)
    finally:
        _close_all(socks)

    # G1b: the SAME cap, but every attempt fired at once instead of one at a time. The accept-
    # -loop drains the whole backlog in a single 'while(true) accept4(...)' pass per epoll wake,-
    # -so a burst of simultaneous connections is exactly the case where an off-by-one in-
    # -AllowConnection's counting would show up as MORE than CAP getting through
    socks = []
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as ex:
            futs = [ex.submit(_open_held, host, port, TRUSTED, "/health",
                              {"X-Forwarded-For": "10.20.80.1"}) for _ in range(12)]
            results = [f.result() for f in futs]

        statuses = [st for st, _, _ in results]
        socks = [s for _, s, _ in results if s is not None]
        succeeded = sum(1 for st in statuses if st == 200)
        p.secure("connection cap: a simultaneous burst still holds at exactly the cap",
                 succeeded == CAP, "expected exactly %d successes, got %d, statuses=%r"
                 % (CAP, succeeded, statuses))
    finally:
        _close_all(socks)

    # G2: distinct identities (varied THIRD octet, so each is a genuinely different /24, not-
    # -the last-octet-only mistake that collapses under NormalizeIp's /24 mask) each get their-
    # -own cap, none of them touch each other
    socks = []
    try:
        all_ok, bad_i = True, None
        for i in range(CAP + 2):
            st, s, _ = _open_held(host, port, TRUSTED, headers={"X-Forwarded-For": "10.20.%d.1" % (i + 10)})
            socks.append(s)
            if st != 200:
                all_ok, bad_i = False, i
        p.check("connection cap: distinct identities each get their own cap",
                all_ok, "identity #%r was refused" % bad_i)
    finally:
        _close_all(socks)

    # G3: untrusted peer - a distinct spoofed XFF per connection cannot dodge the peer's own cap.
    # -Uses UNTRUSTED2, not UNTRUSTED: trust_boundary's G1 already deliberately drains UNTRUSTED's-
    # -own rate-limit bucket, and that state persists (no restart between phases), so reusing it-
    # -here would hit an already-empty bucket before this test ever reaches the connection cap
    socks = []
    try:
        hit, st, seen = None, None, []
        for i in range(CAP + 1):
            st, s, _ = _open_held(host, port, UNTRUSTED2, headers={"X-Forwarded-For": "10.20.%d.1" % (i + 50)})
            socks.append(s)
            seen.append(st)
            if st != 200:
                hit = i
                break
        p.secure("untrusted peer: a distinct spoofed XFF per connection cannot dodge the cap",
                 hit == CAP and st == 503, "expected 503 at connection #%d, got %r at #%r, sequence=%r"
                 % (CAP, st, hit, seen))
    finally:
        _close_all(socks)

    if not _alive(host, port):
        p.failed("CONN_CAP_SERVER_DEAD", "did not answer the dedicated liveness probe")

# Phase: RATE_LIMIT
def phase_rate_limit(ctx):
    cfg = ctx.cfg
    host, port = cfg.host, cfg.port
    p = ctx.phase("rate_limit")

    # G1: burst-then-throttle-then-refill, entirely on ONE held connection (not reconnecting):
    # -exercises the token-bucket arithmetic itself, separate from the reconnect-resistance-
    # -already covered in trust_boundary
    st0, sock, buf = _open_held(host, port, TRUSTED, headers={"X-Forwarded-For": "10.30.1.1"})
    ok_count, hit, st = (1 if st0 == 200 else 0), None, st0
    if sock is not None:
        for i in range(1, BURST):
            st, buf = _send_on(sock, buf, headers={"X-Forwarded-For": "10.30.1.1"})
            if st == 200:
                ok_count += 1
        p.check("rate limit: a full burst succeeds on one held connection",
                ok_count == BURST, "only %d/%d succeeded" % (ok_count, BURST))

        st_over, buf = _send_on(sock, buf, headers={"X-Forwarded-For": "10.30.1.1"})
        p.secure("rate limit: the (burst+1)th request on the same connection is throttled",
                 st_over == 429, "status=%r, expected 429" % st_over)

        # 429 must not force-close a connection the client asked to keep alive: the socket-
        # -should still be usable once the bucket refills
        time.sleep(_REFILL_WAIT)
        st_refilled, buf = _send_on(sock, buf, headers={"X-Forwarded-For": "10.30.1.1"})
        p.secure("429 keeps a keep-alive connection open: it recovers without reconnecting",
                 st_refilled == 200, "status=%r after refill, on the SAME socket" % st_refilled)
        sock.close()
    else:
        p.failed("RATE_LIMIT_CONNECT", "could not open the held connection for G1")

    # G2: a request that explicitly asks for Connection: close, even while being throttled,-
    # -must still actually close - keep-alive-on-limit only applies when the client asked for it
    st0, sock, buf = _open_held(host, port, TRUSTED, headers={"X-Forwarded-For": "10.30.2.1"})
    if sock is not None:
        for _ in range(1, BURST):
            _send_on(sock, buf, headers={"X-Forwarded-For": "10.30.2.1"})
        st_close, buf = _send_on(sock, buf, headers={"X-Forwarded-For": "10.30.2.1"}, close=True)
        p.check("throttled request with Connection: close still gets a response",
                st_close == 429, "status=%r" % st_close)

        # Server should have closed its end: a further read either times out immediately-
        # -with nothing, or the socket errors - either way, nothing more must arrive
        sock.settimeout(1.0)
        try:
            trailing = sock.recv(4096)
        except OSError:
            trailing = b""
        p.check("Connection: close is honored even on a throttled response",
                trailing == b"", "unexpected trailing bytes after close: %r" % trailing[:60])
        sock.close()
    else:
        p.failed("RATE_LIMIT_CONNECT2", "could not open the held connection for G2")

    if not _alive(host, port):
        p.failed("RATE_LIMIT_SERVER_DEAD", "did not answer the dedicated liveness probe")

# Phase: EVICTION
def phase_eviction(ctx):
    cfg = ctx.cfg
    host, port = cfg.host, cfg.port
    p = ctx.phase("eviction")

    # Fill the tracked-identity cap with TRACK_CAP distinct, LIVE (still-open) identities.
    # Live means refCount > 0, which must protect every one of them from eviction
    socks = []
    try:
        all_ok, bad_i = True, None
        for i in range(TRACK_CAP):
            st, s, _ = _open_held(host, port, TRUSTED, headers={"X-Forwarded-For": "10.40.%d.1" % (i + 1)})
            socks.append(s)
            if st != 200:
                all_ok, bad_i = False, i
        p.check("eviction: %d distinct live identities all fit the tracked-identity cap" % TRACK_CAP,
                all_ok, "identity #%r was refused" % bad_i)

        # Every tracked slot is now live: a brand new (TRACK_CAP+1)th identity has nothing-
        # -evictable and must be denied outright, not silently unthrottled
        st_new, sock_new, buf_new = _open_held(host, port, TRUSTED, headers={"X-Forwarded-For": "10.40.99.1"})
        p.secure("eviction: a new identity is denied while every tracked slot is live",
                 st_new == 429, "status=%r, expected 429 (cap saturated with live entries)" % st_new)

        if sock_new is None:
            p.failed("EVICTION_CONNECT", "could not open the (cap+1)th connection")
        else:
            # Free exactly one slot: close the first held identity, dropping its refCount to 0-
            # -(it stays tracked, just no longer live, so it becomes the LRU eviction candidate)
            if socks and socks[0] is not None:
                socks[0].close()
                socks[0] = None
            time.sleep(0.1)

            # Retry on the SAME still-open (thanks to 429 no longer force-closing keep-alive-
            # -connections) socket from the denied identity above: this is the direct regression-
            # -test for the ipAcquired/rateLimiterAcquired split - the first attempt failed to-
            # -track the identity, but a later request on the same connection must retry it-
            # -instead of being permanently stuck once capacity actually frees up
            st_retry, buf_new = _send_on(sock_new, buf_new, headers={"X-Forwarded-For": "10.40.99.1"})
            p.secure("eviction: a previously cap-denied connection recovers once capacity frees up",
                     st_retry == 200, "status=%r, expected 200 after an evictable slot opened up" % st_retry)
            sock_new.close()

        # The evicted identity (10.40.1.1, closed above) is fully forgotten, not half-tracked:-
        # -reconnecting it gets a fresh, working, full-burst bucket rather than an error
        st_evicted = _send_from(host, port, TRUSTED, "GET", "/health", {"X-Forwarded-For": "10.40.1.1"})
        p.check("eviction: a previously evicted identity reconnects cleanly with a fresh bucket",
                st_evicted == 200, "status=%r" % st_evicted)
    finally:
        _close_all(socks)

    if not _alive(host, port):
        p.failed("EVICTION_SERVER_DEAD", "did not answer the dedicated liveness probe")

# Phase: DUALSTACK
def phase_dualstack(ctx):
    cfg = ctx.cfg
    host, port = cfg.host, cfg.port
    p = ctx.phase("dualstack")

    # Regression test for the IPv4-mapped-IPv6 collapse bug: on a dual-stack listener,
    # ResolveIP used to hand back an ordinary IPv4 peer as ::ffff:a.b.c.d, and NormalizeIp's
    # /64 IPv6 mask (coarser than the mapped prefix's fixed 96 bits) folded every such peer
    # into ONE shared identity - one client could then exhaust the cap for everyone
    if not _restart_with_flags(ctx, ["--host", "::"]):
        p.failed("DUALSTACK_BOOT", "server never came back up bound to '::'")
        _restart_with_flags(ctx, [])
        return

    # Peer A and peer B MUST differ in more than their last octet, or NormalizeIp's own /24-
    # -mask would collapse them into one identity regardless of whether the v4-mapped fix-
    # -works at all - this test could never have proven anything with "127.0.0.3"/"127.0.0.4"
    peer_a, peer_b = "127.1.3.3", "127.2.4.4"

    try:
        socks_a, socks_b = [], []
        try:
            seen_a = []
            for _ in range(CAP):
                st, s, _ = _open_held(host, port, peer_a)
                socks_a.append(s)
                seen_a.append(st)
            p.check("dualstack: peer A fills its own cap over the mapped-IPv6 path",
                    all(st == 200 for st in seen_a), "sequence=%r" % seen_a)

            seen_b = []
            for _ in range(CAP):
                st, s, _ = _open_held(host, port, peer_b)
                socks_b.append(s)
                seen_b.append(st)
            p.secure("dualstack: peer B is untouched by peer A's cap (not one shared /64 bucket)",
                     all(st == 200 for st in seen_b),
                     "expected all 200, got sequence=%r (peer A's sequence was %r)" % (seen_b, seen_a))

            st_over, s_over, _ = _open_held(host, port, peer_a)
            socks_a.append(s_over)
            p.check("dualstack: peer A's own (cap+1)th connection is still refused",
                    st_over == 503, "status=%r, expected 503" % st_over)
        finally:
            _close_all(socks_a)
            _close_all(socks_b)

        # A genuine (non-mapped) native IPv6 peer must be enforced correctly too - this-
        # -exercises NormalizeIp's actual v6 branch (/64 mask on a real v6 address) and-
        # -ConnectionLimiter/RequestRateLimiter keyed on type=AF_INET6, neither of which any-
        # -other phase touches at all (::1 is the only v6 loopback address without root, so-
        # -this can't prove two distinct v6 peers are told apart, only that ::1 itself works)
        socks_v6 = []
        try:
            seen_v6 = []
            for _ in range(CAP):
                st, s, _ = _open_held_v6(port)
                socks_v6.append(s)
                seen_v6.append(st)
            p.check("dualstack: native IPv6 (::1) fills its own cap",
                    all(st == 200 for st in seen_v6), "sequence=%r" % seen_v6)

            st_over_v6, s_over_v6, _ = _open_held_v6(port)
            socks_v6.append(s_over_v6)
            p.check("dualstack: native IPv6 (::1) is refused past its own cap too",
                    st_over_v6 == 503, "status=%r, expected 503" % st_over_v6)
        finally:
            _close_all(socks_v6)

        if not _alive(host, port):
            p.failed("DUALSTACK_SERVER_DEAD", "did not answer the dedicated liveness probe")
    finally:
        if not _restart_with_flags(ctx, []):
            p.failed("DUALSTACK_RESTORE_BOOT", "server never came back up after restoring normal host binding")

# Phase: SCALE
def phase_scale(ctx):
    cfg = ctx.cfg
    host, port = cfg.host, cfg.port
    p = ctx.phase("scale")

    # ConnectionLimiter's HashShard has no eviction or cap at all (unlike RequestRateLimiter's
    # BitmapPool-bound pool) - it self-bounds only via erase-on-refCount-0, so its real size
    # tracks however many distinct identities are truly concurrent. This drives real
    # cardinality through it: hundreds of genuinely distinct identities (varying both the
    # second and third octet, not the last-octet mistake made elsewhere this session),
    # confirming the SipHash-keyed table and its grow-only resize neither fail nor slow down
    # pathologically at real scale, not just the handful of addresses every other phase uses
    n = 800
    addrs = ["127.%d.%d.9" % (1 + (i // 40), 1 + (i % 40)) for i in range(n)]

    start = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=40) as ex:
        statuses = list(ex.map(lambda a: _send_from(host, port, a, "GET", "/health"), addrs))
    elapsed = time.time() - start

    failed = [(a, st) for a, st in zip(addrs, statuses) if st != 200]
    p.check("scale: %d distinct identities all succeed" % n, not failed,
            "%d/%d failed, first few: %r" % (len(failed), n, failed[:5]))

    # Generous bound: this is dominated by Python/socket-syscall overhead, not the server -
    # -a pathological O(n^2) HashShard (a broken resize, or a degenerate, non-random hash)-
    # -would blow past this bound by orders of magnitude, not by a little
    p.secure("scale: %d distinct identities complete without pathological slowdown" % n,
             elapsed < 20.0, "took %.2fs for %d identities (expected well under 20s)" % (elapsed, n))

    if not _alive(host, port):
        p.failed("SCALE_SERVER_DEAD", "did not answer the dedicated liveness probe")

# Phase: MULTIWORKER
def phase_multiworker(ctx):
    cfg = ctx.cfg
    host, port = cfg.host, cfg.port
    p = ctx.phase("multiworker")

    # Known, real architectural limitation, not a bug: each worker process (SO_REUSEPORT,
    # confirmed in os_specific/linux/epoll_connection.cpp's Initialize()) owns its own
    # in-memory ConnectionLimiter/RequestRateLimiter. The kernel load-balances connections
    # across worker listening sockets by a hash of the connection 4-tuple, so distinct
    # source ports from the SAME source IP land on different workers - meaning the EFFECTIVE
    # cap for one identity is (configured cap) x (worker count), not the configured cap
    # alone. This demonstrates that directly instead of just asserting it from reading code
    workers = 4
    toml_path = os.path.join(cfg.app_dir, "wfx.toml")
    original = _patch_toml(toml_path, r'(?m)^(\s*worker_processes\s*=\s*)\d+', r'\g<1>%d' % workers)
    try:
        if not _restart(ctx):
            p.failed("MULTIWORKER_BOOT", "server never came back with worker_processes=%d" % workers)
            return

        n = workers * CAP * 4  # comfortable margin so every worker gets several attempts
        socks = []
        try:
            statuses = []
            for _ in range(n):
                st, s, _ = _open_held(host, port, TRUSTED, headers={"X-Forwarded-For": "10.60.1.1"})
                socks.append(s)
                statuses.append(st)

            succeeded = sum(1 for st in statuses if st == 200)
            p.secure("multiworker: one identity gets far more than the configured cap "
                     "(cap x worker count, not one cap shared across workers)",
                     succeeded > CAP,
                     "expected > %d (a single shared cap), got %d successes out of %d attempts, "
                     "statuses=%r" % (CAP, succeeded, n, statuses))
            p.check("multiworker: still bounded by (cap x worker count), not unbounded",
                    succeeded <= workers * CAP,
                    "got %d successes, expected at most %d (%d workers x cap %d)"
                    % (succeeded, workers * CAP, workers, CAP))
        finally:
            _close_all(socks)

        if not _alive(host, port):
            p.failed("MULTIWORKER_SERVER_DEAD", "did not answer the dedicated liveness probe")
    finally:
        _restore_toml(toml_path, original)
        if not _restart(ctx):
            p.failed("MULTIWORKER_RESTORE_BOOT", "server never came back after restoring worker_processes")

# WFX IP audit: real-IP resolution + ConnectionLimiter + RequestRateLimiter
class IpAudit(common.Suite):
    name = "ip_audit"
    description = "WFX IP audit: real-IP resolution, connection cap, rate limit, LRU eviction, dual-stack"
    phases = {
        "trust_boundary": phase_trust_boundary,
        "connection_cap": phase_connection_cap,
        "rate_limit":     phase_rate_limit,
        "eviction":       phase_eviction,
        "dualstack":      phase_dualstack,
        "scale":          phase_scale,
        "multiworker":    phase_multiworker,
    }

    def add_arguments(self, parser):
        parser.set_defaults(ready_timeout=20, wfx_logs=common.logs.CRASH)

if __name__ == "__main__":
    common.run(IpAudit)
