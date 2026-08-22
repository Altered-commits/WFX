#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX endpoint audit: the raw WFX::Endpoint<> primitive
#
# Drives include/wfx/endpoint/base.hpp through two hand-rolled protocols served by
# primitive_upstream.py, one multiplexed (mux) and one not (solo). Two are needed
# because half the primitive's surface is unreachable through a multiplexed
# endpoint. See README.md for that reasoning and for what each phase proves.
#
# Usage:
#   python3 endpoint_audit.py                       # all phases
#   python3 endpoint_audit.py --phase solo_streaming
#   python3 endpoint_audit.py --list-phases
#
# Exit codes:
#   0   all phases passed
#   1   correctness failure, or the worker died during the run
#   2   security finding: one caller receiving another's response, a refused
#       handshake still being served, a pinned connection shared with anyone else,
#       an unsolicited push desyncing the framing, a failed TLS upgrade leaving
#       the endpoint usable in plaintext, or memory growing with the total
#       response size rather than the chunk size
#   3   WFX never answered /health, so nothing was exercised

import os
import subprocess
import sys
import threading
import time

# Suites are run directly, so tests/ has to be on the path before common is importable
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import common
from common import net, term

HERE = os.path.dirname(os.path.abspath(__file__))

# EndpointStatus, mirrors shared/abis/types.hpp, keep in sync. Only the three this
# suite asserts on by name are listed; every other failure is checked as "not success"
EP_SUCCESS           = 0
EP_INTERNAL          = 10  # where a co_return EpFatal from onConnect lands: the coroutine
                           # only ever answers EpReady or EpFatal, so the engine's generic
                           # connect-failure funnel classifies it, not a per-cause status
EP_HANDSHAKE_TIMEOUT = 13

# Waits the engine's own budgets dictate. Its timeout timer ticks every 5s
# (INVOKE_TIMEOUT_COOLDOWN), so an N-second budget can take up to N+5 to be noticed
IDLE_RECYCLE_WAIT    = 11.0  # idleTimeoutSeconds 5, plus a tick, plus slack
AUX_RECLAIM_WAIT     = 5.5   # connectTimeoutSeconds 5 reclaiming a leaked side connection
REQUEST_TIMEOUT_WAIT = 17.0  # requestTimeoutSeconds 10, plus a tick, plus slack

# The mock upstream
class Mock:
    """primitive_upstream.py: the mux and solo listeners plus an HTTP counter plane."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.proc = None

    def start(self):
        cmd = [sys.executable, os.path.join(HERE, "primitive_upstream.py"),
               "--host", self.cfg.host,
               "--control-port", str(self.cfg.control_port),
               "--mux-port", str(self.cfg.mux_port),
               "--solo-port", str(self.cfg.solo_port)]
        term.log("mock", "starting: %s" % " ".join(cmd))
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

        for _ in range(50):
            if self.ping():
                term.log("mock", term.green("up on %s:%d" % (self.cfg.host, self.cfg.control_port)))
                return
            time.sleep(0.1)

        raise RuntimeError("mock upstream never came up on port %d" % self.cfg.control_port)

    def _ctl(self, path):
        raw = net.send(self.cfg.host, self.cfg.control_port, net.request("GET", path),
                       rtimeout=2.0, ctimeout=2.0)
        return net.body(raw) if raw else b""

    def ping(self):
        return self._ctl("/ctl/ping") == b"pong"

    def mux_conns(self):
        """Connections the mux listener accepted, the evidence for what got shared."""
        try:
            return int(self._ctl("/ctl/mux/conns"))
        except ValueError:
            return -1

    def cancels(self):
        """(cancels seen on solo side connections, the connId the last one named)."""
        try:
            count, last = self._ctl("/ctl/solo/cancels").split()
            return int(count), int(last)
        except ValueError:
            return -1, -1

    def cancels_reset(self):
        self._ctl("/ctl/solo/cancels/reset")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

# Driving WFX
#
# Every route answers 200 with a JSON body carrying at least "ep", the EndpointStatus
# as an int. A None here means WFX itself never answered, which is a different (and
# worse) failure than an outbound call that failed cleanly
def _json(cfg, method, path, headers=None, rtimeout=15.0):
    return net.get_json(cfg.host, cfg.port, method, path, headers, rtimeout=rtimeout)

def mux_call(cfg, key="hello", mux="good", rtimeout=8.0):
    return _json(cfg, "GET", "/mux/call", {"X-Mux": mux, "X-Key": key}, rtimeout)

def mux_disconnects(cfg):
    return _json(cfg, "GET", "/mux/disconnects") or {}

def mux_disconnects_reset(cfg):
    _json(cfg, "POST", "/mux/disconnects/reset")

def solo_get(cfg, key="hello", solo="good", rtimeout=15.0):
    return _json(cfg, "GET", "/solo/get", {"X-Solo": solo, "X-Key": key}, rtimeout)

def solo_stream(cfg, count=10, size=64, mode="server", stop=0, rtimeout=60.0):
    return _json(cfg, "GET", "/solo/stream",
                 {"X-Mode": mode, "X-Count": str(count), "X-Size": str(size), "X-Stop": str(stop)},
                 rtimeout)

def solo_reserve(cfg, n=3, release="late", rtimeout=20.0):
    return _json(cfg, "GET", "/solo/reserve", {"X-N": str(n), "X-Release": release}, rtimeout)

def solo_reserve_pair(cfg, rtimeout=20.0):
    return _json(cfg, "GET", "/solo/reserve/pair", rtimeout=rtimeout)

def solo_reserve_stream(cfg, count=6, rtimeout=30.0):
    return _json(cfg, "GET", "/solo/reserve/stream", {"X-Count": str(count)}, rtimeout)

def push_stats(cfg):
    return _json(cfg, "GET", "/solo/push") or {}

def push_reset(cfg):
    _json(cfg, "POST", "/solo/push/reset")

def abort_stats(cfg):
    return _json(cfg, "GET", "/solo/abort") or {}

def worker_rss(cfg):
    return (_json(cfg, "GET", "/rss") or {}).get("rss", 0)

def metrics(cfg):
    return _json(cfg, "GET", "/metrics", rtimeout=4.0) or {}

def metric(snapshot, field):
    """Sums one counter across every endpoint slot.

    Several instances share a host, so a slot cannot be mapped back to one instance;
    the aggregate delta either side of a driven call is what is assertable.
    """
    return sum(e.get(field, 0) for e in snapshot.get("endpoints", []))

def latency_metric(snapshot, field):
    return sum((e.get("latency") or {}).get(field, 0) for e in snapshot.get("endpoints", []))

# Abandoning a request mid-flight is what makes onAbort fire: the inbound connection
# is dropped while the outbound call is still in the air
def abandon_solo_get(cfg, headers, hold=0.15):
    net.send_and_abandon(cfg.host, cfg.port, net.request("GET", "/solo/get", headers), hold=hold)

def abandon_solo_stream(cfg, headers, hold=0.3):
    net.send_and_abandon(cfg.host, cfg.port, net.request("GET", "/solo/stream", headers), hold=hold)

# Predicates
def is_ok(r):
    """WFX answered and the outbound call succeeded."""
    return bool(r) and r.get("ep") == EP_SUCCESS

def is_err(r):
    """WFX answered, but the outbound call failed cleanly."""
    return bool(r) and r.get("ep") != EP_SUCCESS

def is_errc(r, code):
    return bool(r) and r.get("ep") == code

def answered(r):
    """WFX produced a well-formed answer at all, so it neither crashed nor hung."""
    return r is not None

def wfx_healthy(cfg):
    raw = net.send(cfg.host, cfg.port, net.request("GET", "/health"), rtimeout=2.0, ctimeout=2.0)
    return bool(raw) and net.status(raw) == 200

# Concurrency
def in_background(fn, *args, **kwargs):
    """Runs one driver on its own thread. The returned callable blocks for its result."""
    box = {}

    def run():
        box["r"] = fn(*args, **kwargs)

    thread = threading.Thread(target=run)
    thread.start()

    def join():
        thread.join()
        return box.get("r")

    return join

def in_parallel(fn, n, stagger=0.01):
    """Runs fn(i) for i in range(n) at once, results in submission order.

    The small stagger spreads the inbound connection burst, so WFX's accept path is
    not hit by n simultaneous SYNs, which used to drop a few of them as None.
    """
    out = [None] * n

    def worker(i):
        out[i] = fn(i)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(n)]
    for thread in threads:
        thread.start()
        if stagger:
            time.sleep(stagger)
    for thread in threads:
        thread.join()

    return out

# PHASE: mux_handshake
def phase_mux_handshake(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("mux_handshake")

    mux_disconnects_reset(cfg)

    r = mux_call(cfg, key="alpha")
    p.check("onConnect accepted: the call is served and its value round-trips",
            is_ok(r) and r.get("value") == "alpha", "r=%r" % r)

    r = mux_call(cfg, mux="bad")
    p.check("onConnect rejected: fails cleanly, the request is never served",
            is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)

    r = mux_call(cfg, mux="reset")
    p.check("onConnect dropped mid-handshake: fails cleanly, the request is never served",
            is_errc(r, EP_INTERNAL), "r=%r" % r, security=True)

    t0 = time.time()
    r = mux_call(cfg, mux="slow", rtimeout=20.0)
    elapsed = time.time() - t0
    p.check("onConnect stalled: surfaces as a handshake timeout, not a hang",
            is_errc(r, EP_HANDSHAKE_TIMEOUT) and elapsed < 18.0,
            "elapsed %.1fs r=%r" % (elapsed, r))

    p.check("worker survives every onConnect failure", wfx_healthy(cfg) and mock.ping())

    # onDisconnect has to report the reason the scenario actually produced, not just
    # that something disconnected
    counters = mux_disconnects(cfg)
    p.check("onDisconnect: the stalled handshake is reported as a handshake timeout",
            counters.get("handshake", 0) >= 1, "counters=%r" % counters)
    p.check("onDisconnect: the rejected and dropped handshakes are reported as errors",
            counters.get("error", 0) >= 2, "counters=%r" % counters)

    # A rejected, dropped or timed-out handshake must not poison the pool for the
    # next legitimate connection attempt
    r = mux_call(cfg, key="still-good")
    p.check("a fresh connection still works after every prior handshake failure",
            is_ok(r) and r.get("value") == "still-good", "r=%r" % r)

# PHASE: mux_multiplex
def phase_mux_multiplex(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("mux_multiplex")

    # Requests deliberately resolved out of order: every caller must get back exactly
    # its own value, never another caller's, which is the bleed check
    delays = [0.30, 0.02, 0.18, 0.05, 0.25, 0.01, 0.12, 0.28, 0.08, 0.22, 0.03, 0.15]

    conns_before = mock.mux_conns()
    joins = [in_background(mux_call, cfg, key="sleep:%.2f:tok%d" % (delays[i], i), rtimeout=10.0)
             for i in range(len(delays))]
    got = [join() for join in joins]
    conns_after = mock.mux_conns()

    p.check("every concurrent request on one connection succeeds",
            all(is_ok(r) for r in got), "results=%r" % got)

    matched = all(is_ok(got[i]) and got[i].get("value") == "tok%d" % i for i in range(len(delays)))
    p.check("no cross-request bleed: replies are matched by id, not by arrival order", matched,
            "expected tok0..tok%d, got %r" % (len(delays) - 1,
                                              [r.get("value") if r else None for r in got]),
            security=True)

    p.check("concurrent requests share one physical connection",
            conns_after - conns_before <= 1,
            "mux connections before=%d after=%d, expected at most one more"
            % (conns_before, conns_after))

    # Nothing rides the connection from here on, so the idle timer is free to fire
    time.sleep(IDLE_RECYCLE_WAIT)
    counters = mux_disconnects(cfg)
    p.check("onDisconnect: an idle connection is reported as an idle timeout",
            counters.get("idle", 0) >= 1, "counters=%r" % counters)

# PHASE: mux_soak
def phase_mux_soak(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("mux_soak")

    sequential = 2500
    conns_before = mock.mux_conns()

    failed_at, failure = None, None
    for i in range(sequential):
        key = "k%d" % i
        r = mux_call(cfg, key=key, rtimeout=10.0)
        if not (is_ok(r) and r.get("value") == key):
            failed_at, failure = i, r
            break

    conns_after = mock.mux_conns()

    p.check("%d sequential multiplexed requests all succeeded" % sequential, failed_at is None,
            "first failure at request #%s: %r, is the multiplexed write buffer reclaimed?"
            % (failed_at, failure))

    p.check("sustained load stayed on one pooled connection", conns_after - conns_before <= 1,
            "mux connections before=%d after=%d, expected at most one more"
            % (conns_before, conns_after))

    # Repeated concurrent waves churn the pending-stream map and the per-stream parse
    # state under sustained concurrency, many streams in flight at once, over and
    # over. One 12-request burst touches each of those paths once; this hammers them,
    # so a use-after-free or a leak on the completion path surfaces here, either as a
    # mismatch or as the sanitizer gate killing the worker mid-wave
    waves, width = 30, 16
    wave_failures = 0
    for wave in range(waves):
        expect = set("w%d_%d" % (wave, i) for i in range(width))
        joins = [in_background(mux_call, cfg, key=key, rtimeout=10.0) for key in expect]
        got = [join() for join in joins]
        if set(r.get("value") for r in got if is_ok(r)) != expect:
            wave_failures += 1

    p.check("%d concurrent waves of %d all delivered their own values" % (waves, width),
            wave_failures == 0, "%d of %d waves had a missing or mismatched response"
            % (wave_failures, waves), security=True)

    p.check("worker healthy after the soak", wfx_healthy(cfg) and mock.ping())

# PHASE: solo_pinning
def phase_solo_pinning(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("solo_pinning")

    # Every request on one reservation must land on the same physical connection.
    # connId comes from the mock's accept counter, so this is observed, not assumed
    r = solo_reserve(cfg, n=4, release="late")
    p.check("reserve: a slot is acquired", bool(r) and r.get("reserved") == 1, "r=%r" % r)
    p.check("reserve: all four requests ran on one connection",
            bool(r) and r.get("same") == 1 and r.get("conn", 0) > 0, "r=%r" % r)
    p.check("reserve: every pinned request succeeded",
            bool(r) and r.get("last") == EP_SUCCESS, "r=%r" % r)

    # Two live reservations must be two different connections. Collapsed onto one, a
    # transaction opened on A would be visible to B
    r = solo_reserve_pair(cfg)
    p.check("reserve: two simultaneous reservations are both granted",
            bool(r) and r.get("a_ok") == 1 and r.get("b_ok") == 1, "r=%r" % r)
    p.check("reserve: distinct reservations get distinct connections",
            bool(r) and r.get("distinct") == 1, "r=%r" % r, security=True)
    p.check("reserve: byte-identical payloads on two pins still use both connections",
            bool(r) and r.get("sa") == EP_SUCCESS and r.get("sb") == EP_SUCCESS
            and r.get("conn_a") != r.get("conn_b"), "r=%r" % r, security=True)

    # Explicit-early, destructor-late and double release must all leave the pool
    # usable. A double release that cleared the same bitmap bit twice would corrupt
    # the pool for every later caller
    for mode in ("early", "late", "double"):
        r = solo_reserve(cfg, n=2, release=mode)
        p.check("reserve: release=%s completes cleanly" % mode,
                bool(r) and r.get("reserved") == 1 and r.get("same") == 1, "r=%r" % r)

    p.check("reserve: the pool is still healthy after every release pattern",
            is_ok(solo_get(cfg, key="after-release")), "")

    # Reservations have to come back to the pool. SoloGood leaves exactSlots off, so
    # its connLimit of 4 rounds up to a full 64-bit bitmap word and the pool really
    # holds 64 slots; a run longer than that is what a leaked reservation starves
    reservations = 70
    starved = False
    for _ in range(reservations):
        r = solo_reserve(cfg, n=1, release="early")
        if not r or r.get("reserved") != 1:
            starved = True
            break
    p.check("reserve: %d sequential reservations never exhaust the pool" % reservations,
            not starved, "a reservation was refused, so an earlier one leaked its slot")

    # Pinning and streaming have to compose, with every chunk coming off the reserved
    # slot rather than a pooled one
    r = solo_reserve_stream(cfg, count=6)
    p.check("reserve+stream: the reservation is held for the whole stream",
            bool(r) and r.get("reserved") == 1 and r.get("chunks") == 6, "r=%r" % r)
    p.check("reserve+stream: every chunk came off the same pinned connection",
            bool(r) and r.get("same") == 1, "r=%r" % r, security=True)

    p.check("worker healthy after the pinning phase", wfx_healthy(cfg) and mock.ping())

# PHASE: solo_push
def phase_solo_push(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("solo_push")

    # Baseline: pushes land on an idle slot and are decoded there, rather than being
    # mistaken for the response to some later request
    push_reset(cfg)
    r = solo_get(cfg, key="push:5")
    p.check("push: the request that provoked them is still answered correctly", is_ok(r), "r=%r" % r)
    time.sleep(0.6)
    stats = push_stats(cfg)
    p.check("push: all five unsolicited messages reached onPush", stats.get("count", 0) >= 5,
            "stats=%r" % stats)
    p.check("push: the connection is still usable afterwards",
            is_ok(solo_get(cfg, key="after-push")), "")

    # A push arriving WHILE a request is in flight must be consumed by parse, never
    # routed to onPush. Getting that split wrong desyncs framing, because the push
    # bytes would be read as part of the response
    push_reset(cfg)
    before = push_stats(cfg).get("count", 0)
    r = solo_get(cfg, key="pushinflight")
    p.check("push: an in-flight push is handled by parse and the request still answered",
            answered(r), "r=%r" % r, security=True)
    time.sleep(0.4)
    after = push_stats(cfg).get("count", 0)
    p.check("push: an in-flight push never reaches onPush", after == before,
            "onPush count before=%d after=%d" % (before, after), security=True)

    # A partial message with no trailing newline makes onPush report consumed=0 every
    # time. The engine has to park and wait for more bytes, rather than spinning on a
    # buffer it cannot drain or wedging the slot against future use
    push_reset(cfg)
    t0 = time.time()
    r = solo_get(cfg, key="pushpartial")
    p.check("push: the request behind a partial push is still answered", answered(r), "r=%r" % r)
    time.sleep(0.5)
    elapsed = time.time() - t0
    p.check("push: a partial push neither spins nor hangs the worker",
            wfx_healthy(cfg) and elapsed < 10.0, "elapsed %.1fs" % elapsed, security=True)

    # Undecodable bytes make onPush return false and the engine close the slot. The
    # endpoint has to recover rather than stay poisoned
    push_reset(cfg)
    r = solo_get(cfg, key="pushgarbage")
    p.check("push: the request behind an undecodable push is still answered", answered(r),
            "r=%r" % r)
    time.sleep(0.5)
    stats = push_stats(cfg)
    p.check("push: an undecodable push is rejected by the handler", stats.get("rejects", 0) >= 1,
            "stats=%r" % stats)
    p.check("push: the endpoint recovers after a rejected push",
            is_ok(solo_get(cfg, key="after-garbage")), "", security=True)

    # Flood: the engine has to drain 2000 pushes incrementally rather than buffering
    # the whole burst
    push_reset(cfg)
    flood = 2000
    rss_before = worker_rss(cfg)
    r = solo_get(cfg, key="pushflood:%d" % flood)
    p.check("push: the request behind a flood is still answered", answered(r), "r=%r" % r)
    time.sleep(1.5)
    stats = push_stats(cfg)
    rss_after = worker_rss(cfg)
    growth = rss_after - rss_before

    p.check("push: the flood was decoded", stats.get("count", 0) >= flood * 3 // 4, "stats=%r" % stats)
    # The flood is roughly 140KB on the wire, so several MB of RSS growth means it
    # accumulated instead of draining
    p.check("push: the flood did not balloon worker memory",
            rss_before == 0 or growth < 8 * 1024 * 1024,
            "rss %d -> %d, +%d bytes" % (rss_before, rss_after, growth), security=True)

    p.check("worker healthy after the push phase", wfx_healthy(cfg) and mock.ping())

# PHASE: solo_streaming
def phase_solo_streaming(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("solo_streaming")

    # Server-driven (CHUNK_READY): the mock keeps sending until END
    r = solo_stream(cfg, count=10, size=64, mode="server")
    p.check("stream: server-driven delivered all ten chunks",
            bool(r) and r.get("chunks") == 10 and r.get("done") == 1, "r=%r" % r)
    p.check("stream: server-driven moved 10 x 64 bytes", bool(r) and r.get("bytes") == 640,
            "r=%r" % r)

    # Cursor-driven (CHUNK_READY_FETCH): the mock sends nothing until asked, so this
    # only completes if the engine re-serialized a continuation each time. Same shape
    # as a Postgres Execute, a Cassandra paging_state, a Redis SCAN cursor
    r = solo_stream(cfg, count=10, size=64, mode="fetch")
    p.check("stream: cursor-driven delivered all ten chunks",
            bool(r) and r.get("chunks") == 10 and r.get("done") == 1, "r=%r" % r)
    p.check("stream: cursor-driven moved the same 10 x 64 bytes",
            bool(r) and r.get("bytes") == 640, "r=%r" % r)

    # Both families over identical data must fold to the same checksum, which is what
    # proves chunk boundaries neither drop nor duplicate bytes
    served = solo_stream(cfg, count=8, size=100, mode="server")
    fetched = solo_stream(cfg, count=8, size=100, mode="fetch")
    p.check("stream: the two chunk families agree byte for byte",
            bool(served) and bool(fetched) and served.get("checksum") == fetched.get("checksum")
            and served.get("checksum", 0) != 0, "server=%r fetch=%r" % (served, fetched))

    # The memory assertion: same chunk size, 200x the chunk count. An engine that
    # accumulated chunks instead of reusing one output object would grow peak RSS
    # with the total size rather than holding flat
    small = solo_stream(cfg, count=50, size=1024, mode="server", rtimeout=60.0)
    rss_small = worker_rss(cfg)
    big = solo_stream(cfg, count=10000, size=1024, mode="server", rtimeout=120.0)
    rss_big = worker_rss(cfg)
    growth = rss_big - rss_small

    p.check("stream: the 50-chunk baseline completed", bool(small) and small.get("chunks") == 50,
            "r=%r" % small)
    p.check("stream: the 10000-chunk response completed",
            bool(big) and big.get("chunks") == 10000 and big.get("done") == 1, "r=%r" % big)
    p.check("stream: 10000 chunks carried the full ~10MB of payload",
            bool(big) and big.get("bytes", 0) >= 10000 * 1024,
            "bytes=%r" % (big or {}).get("bytes"))
    p.check("stream: peak memory tracks the chunk size, not the total size",
            rss_small == 0 or growth < 6 * 1024 * 1024,
            "rss %d -> %d, +%d bytes across 200x more data" % (rss_small, rss_big, growth),
            security=True)

    # Abandonment: the caller stops reading after 3 of 500 chunks, so the slot has to
    # be reclaimed rather than stranded holding a half-drained response
    r = solo_stream(cfg, count=500, size=64, mode="server", stop=3, rtimeout=60.0)
    p.check("stream: a stream abandoned after three chunks stops there",
            bool(r) and r.get("chunks") == 3, "r=%r" % r)
    p.check("stream: the endpoint is usable after an abandoned stream",
            is_ok(solo_get(cfg, key="after-abandon")), "", security=True)
    r = solo_stream(cfg, count=5, size=32, mode="server")
    p.check("stream: a fresh stream works after an abandoned one",
            bool(r) and r.get("chunks") == 5 and r.get("done") == 1, "r=%r" % r)

    # Zero-chunk response: END arrives with no CHUNK at all
    r = solo_stream(cfg, count=0, size=64, mode="server")
    p.check("stream: an empty stream terminates cleanly",
            bool(r) and r.get("chunks") == 0 and r.get("done") == 1, "r=%r" % r)

    # Concurrent streams must not cross-deliver. Each asks for a distinct chunk count,
    # so a swapped delivery shows up as the wrong count
    counts = (4, 7, 11, 15)
    joins = [in_background(solo_stream, cfg, count=n, size=32, mode="server", rtimeout=60.0)
             for n in counts]
    got = [join() for join in joins]
    p.check("stream: four concurrent streams all completed",
            all(bool(r) and r.get("done") == 1 for r in got), "got=%r" % got)
    p.check("stream: each concurrent stream got its OWN chunk count",
            [r.get("chunks") for r in got if r] == list(counts), "got=%r" % got, security=True)

    p.check("worker healthy after the streaming phase", wfx_healthy(cfg) and mock.ping())

# PHASE: solo_upgrade
def phase_solo_upgrade(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("solo_upgrade")

    # The server answers "N" to an endpoint whose protocol requires TLS. Continuing in
    # plaintext here is the CVE-2015-3152 / CVE-2025-49146 failure mode
    t0 = time.time()
    r = solo_get(cfg, key="x", solo="downgrade", rtimeout=20.0)
    elapsed = time.time() - t0
    p.check("upgrade: a refused upgrade fails closed, never continues in plaintext",
            is_err(r), "r=%r" % r, security=True)
    p.check("upgrade: the refusal is prompt rather than a hang", elapsed < 18.0,
            "elapsed %.1fs" % elapsed)

    # The server answers "S" then sends junk instead of a ServerHello
    t0 = time.time()
    r = solo_get(cfg, key="x", solo="tlsgarbage", rtimeout=20.0)
    elapsed = time.time() - t0
    p.check("upgrade: a garbage handshake fails cleanly", is_err(r), "r=%r" % r, security=True)
    p.check("upgrade: a garbage handshake does not hang the worker",
            elapsed < 18.0 and wfx_healthy(cfg), "elapsed %.1fs" % elapsed)

    # A failed upgrade must not poison the endpoint for unrelated traffic
    p.check("upgrade: the plaintext endpoint is unaffected by the failed upgrades",
            is_ok(solo_get(cfg, key="after-upgrade-failures")), "", security=True)

    p.check("worker healthy after the upgrade phase", wfx_healthy(cfg) and mock.ping())

# PHASE: solo_abort
def phase_solo_abort(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("solo_abort")

    # Happy path: the cancel is delivered, names the right connection, and the primary
    # connection survives rather than being force-closed
    mock.cancels_reset()
    warmup = solo_get(cfg, key="warmup", solo="abort")
    p.check("abort: the warm-up request succeeds", is_ok(warmup), "r=%r" % warmup)
    aborted_conn = warmup.get("conn", -1) if warmup else -1

    abandon_solo_get(cfg, {"X-Solo": "abort", "X-Key": "slow:1.0"})
    time.sleep(0.4)  # the side connection dials and sends well before the 1.0s reply
    count, last_id = mock.cancels()
    p.check("abort: exactly one cancel is delivered", count == 1, "count=%d" % count)
    p.check("abort: the cancel names the connection actually being aborted",
            last_id == aborted_conn, "cancel named connId=%d, expected %d" % (last_id, aborted_conn),
            security=True)

    time.sleep(1.0)  # let the now-orphaned reply land and return the slot to the pool
    r = solo_get(cfg, key="after-abort", solo="abort")
    p.check("abort: the connection is reused afterwards, so it was never force-closed",
            is_ok(r) and r.get("conn") == aborted_conn,
            "r=%r expected connId=%d" % (r, aborted_conn))

    # auxConnLimit 0: onAbort still runs, and OpenSideConnection has to fail gracefully
    mock.cancels_reset()
    before = abort_stats(cfg)
    abandon_solo_get(cfg, {"X-Solo": "abortnoaux", "X-Key": "slow:1.0"})
    time.sleep(0.4)
    after = abort_stats(cfg)
    count, _ = mock.cancels()
    p.check("abort: with no aux capacity no cancel is sent", count == 0, "count=%d" % count)
    p.check("abort: onAbort still ran and recorded the side-open failure",
            after.get("side_failures", 0) - before.get("side_failures", 0) == 1,
            "before=%r after=%r" % (before, after))
    time.sleep(1.0)
    p.check("abort: the worker is healthy with no aux capacity", wfx_healthy(cfg) and mock.ping())

    # Concurrent aborts on one endpoint's aux pool. SoloAbort leaves exactSlots off, so
    # its auxConnLimit of 1 rounds up to a full 64-bit bitmap word and both side
    # connections comfortably fit. What this proves is that concurrent aborts do not
    # corrupt or fight over each other, not that the configured limit is a hard cap
    mock.cancels_reset()
    before = abort_stats(cfg)
    in_parallel(lambda i: abandon_solo_get(cfg, {"X-Solo": "abort", "X-Key": "slow:1.5"}, hold=0.05),
                2, stagger=0.03)
    time.sleep(2.0)
    after = abort_stats(cfg)
    opens = after.get("side_opens", 0) - before.get("side_opens", 0)
    failures = after.get("side_failures", 0) - before.get("side_failures", 0)
    count, _ = mock.cancels()
    p.check("abort: two concurrent aborts on one endpoint both open cleanly",
            opens == 2 and failures == 0, "side opens=%d failures=%d" % (opens, failures))
    p.check("abort: both cancels reached the mock", count == 2, "count=%d" % count)
    p.check("worker healthy after concurrent aborts", wfx_healthy(cfg) and mock.ping())

    # A leaked side connection (onAbort chooses not to Close it) must never hang or
    # crash the worker, and connectTimeoutSeconds has to be the working safety net
    # that reclaims it rather than a config value nobody exercises
    mock.cancels_reset()
    before = abort_stats(cfg)
    abandon_solo_get(cfg, {"X-Solo": "abort", "X-Key": "slow:1.0", "X-Leak": "1"}, hold=0.1)
    time.sleep(0.3)
    after = abort_stats(cfg)
    count, _ = mock.cancels()
    p.check("abort: a leaking onAbort still opens its side connection",
            after.get("side_opens", 0) - before.get("side_opens", 0) == 1,
            "before=%r after=%r" % (before, after))
    p.check("abort: the leaked connection's cancel still arrived", count == 1, "count=%d" % count)

    time.sleep(AUX_RECLAIM_WAIT)
    p.check("abort: the endpoint is fully usable once the leaked slot self-heals",
            is_ok(solo_get(cfg, key="after-leak", solo="abort")), "")
    p.check("worker healthy after the leak-reclaim sequence", wfx_healthy(cfg) and mock.ping())

    # Regression guard: the client disconnects while onConnect is still in flight.
    # onAbort must not fire there, since it would steal onConnect's own asyncData
    # mid-await and strand its coroutine forever. Force-closing instead is correct,
    # because no request ever reached the backend
    mock.cancels_reset()
    before = abort_stats(cfg)
    abandon_solo_get(cfg, {"X-Solo": "abortmidconnect", "X-Key": "x"})
    time.sleep(1.5)  # past the mock's 1.0s AUTH stall
    after = abort_stats(cfg)
    count, _ = mock.cancels()
    p.check("abort: onAbort never fires while onConnect is still in flight",
            after.get("runs", 0) == before.get("runs", 0), "before=%r after=%r" % (before, after),
            security=True)
    p.check("abort: no cancel is sent, nothing had reached the backend yet", count == 0,
            "count=%d" % count)
    p.check("abort: the endpoint is healthy after a mid-connect abort",
            is_ok(solo_get(cfg, key="after-midconnect", solo="abortmidconnect")), "")
    p.check("worker healthy after the mid-connect abort race", wfx_healthy(cfg) and mock.ping())

    # Hostile onAbort: closes its side connection three times and keeps the handle
    # afterwards. A real protocol author will get this wrong at least once, so the
    # engine must survive it rather than double-free the aux slot or corrupt whichever
    # connection that slot is handed to next
    mock.cancels_reset()
    abandon_solo_get(cfg, {"X-Solo": "abortbadclose", "X-Key": "slow:0.8"}, hold=0.1)
    time.sleep(1.3)
    count, _ = mock.cancels()
    p.check("abort: a double and triple Close on a side connection never crashes the worker",
            wfx_healthy(cfg) and mock.ping(), "", security=True)
    p.check("abort: the hostile onAbort still delivered its one cancel", count == 1,
            "count=%d" % count)
    p.check("abort: the endpoint is still usable after that misuse",
            is_ok(solo_get(cfg, key="after-badclose", solo="abortbadclose")), "")

    # Scope cut: once a request is already streaming, past its first chunk, a client
    # disconnect still force-closes. onAbort is defined for single-slot in-flight
    # requests only, never for a stream mid-delivery
    mock.cancels_reset()
    before = abort_stats(cfg)
    abandon_solo_stream(cfg, {"X-Solo": "abort", "X-Count": "4", "X-StallAfter": "1",
                              "X-StallMs": "2000"})
    time.sleep(2.5)
    after = abort_stats(cfg)
    count, _ = mock.cancels()
    p.check("abort: onAbort never fires for a slot that is already streaming",
            after.get("runs", 0) == before.get("runs", 0), "before=%r after=%r" % (before, after),
            security=True)
    p.check("abort: an abandoned stream sends no cancel, it force-closes instead", count == 0,
            "count=%d" % count)
    p.check("abort: the endpoint is healthy after abandoning a stream mid-flight",
            is_ok(solo_get(cfg, key="after-stream-abort", solo="abort")), "")

    # Cross-endpoint isolation: every endpoint owns its own aux pool, so one endpoint
    # holding its aux slot must not block or interfere with a different endpoint's
    mock.cancels_reset()
    before = abort_stats(cfg)
    abandon_solo_get(cfg, {"X-Solo": "abort", "X-Key": "slow:1.5", "X-Leak": "1"}, hold=0.05)
    time.sleep(0.2)  # let the first endpoint's onAbort claim and hold its aux slot
    abandon_solo_get(cfg, {"X-Solo": "abortbadclose", "X-Key": "slow:0.5"}, hold=0.05)
    time.sleep(0.8)
    after = abort_stats(cfg)
    count, _ = mock.cancels()
    p.check("abort: one endpoint holding its aux slot never blocks another endpoint's",
            after.get("side_opens", 0) - before.get("side_opens", 0) == 2,
            "before=%r after=%r" % (before, after), security=True)
    p.check("abort: both endpoints' cancels arrived", count == 2, "count=%d" % count)
    time.sleep(AUX_RECLAIM_WAIT)  # let the leaked slot self-heal before anything else runs
    p.check("worker healthy after the cross-endpoint isolation check",
            wfx_healthy(cfg) and mock.ping())

    # Soak: rapid abort and reclaim cycles must never leak a primary slot, an aux slot
    # or a coroutine frame. The in-use gauge returning to zero is the same invariant
    # the metrics phase checks, here under abort load
    mock.cancels_reset()
    soak = 20
    for _ in range(soak):
        abandon_solo_get(cfg, {"X-Solo": "abort", "X-Key": "slow:0.05"}, hold=0.01)
        time.sleep(0.03)
    time.sleep(1.0)
    count, _ = mock.cancels()
    p.check("abort: a soak of %d rapid aborts recorded that many cancels" % soak, count == soak,
            "count=%d expected=%d" % (count, soak))
    in_use = metric(metrics(cfg), "slots_in_use")
    p.check("abort: the in-use slot gauge is back to zero after the soak", in_use == 0,
            "expected 0, got %d" % in_use)
    p.check("worker healthy after the abort soak", wfx_healthy(cfg) and mock.ping())

    # A backend that never responds, even after the cancel: onAbort still sends its
    # cancel normally, since it has no idea whether one "worked", and
    # requestTimeoutSeconds is what eventually force-closes the already-aborted slot.
    # onAbort adds no timer plumbing of its own, this is the budget every request gets
    mock.cancels_reset()
    warmup = solo_get(cfg, key="warmup-timeout", solo="abort")
    p.check("abort: the warm-up before the timeout check succeeds", is_ok(warmup), "r=%r" % warmup)
    stale_conn = warmup.get("conn", -1) if warmup else -1

    before = metrics(cfg)
    abandon_solo_get(cfg, {"X-Solo": "abort", "X-Key": "slow:30"})
    time.sleep(0.5)
    count, _ = mock.cancels()
    p.check("abort: the cancel is still sent even though the backend will never reply",
            count == 1, "count=%d" % count)

    time.sleep(REQUEST_TIMEOUT_WAIT)
    after = metrics(cfg)
    p.check("abort: a never-responding backend is recorded as a request timeout",
            metric(after, "request_timeouts") - metric(before, "request_timeouts") >= 1,
            "request_timeouts before=%d after=%d"
            % (metric(before, "request_timeouts"), metric(after, "request_timeouts")))

    r = solo_get(cfg, key="after-timeout", solo="abort")
    p.check("abort: the timed-out slot was force-closed, so the next call gets a fresh one",
            is_ok(r) and r.get("conn") != stale_conn, "r=%r, timed-out connId=%d" % (r, stale_conn))
    p.check("worker healthy after the never-responding backend timed out",
            wfx_healthy(cfg) and mock.ping())

# PHASE: metrics
def phase_metrics(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("metrics")

    start = metrics(cfg)
    p.check("metrics: latency histograms are enabled", start.get("latency_enabled") is True,
            "expected [Metrics] latency = true in wfx.local.toml, got %r"
            % start.get("latency_enabled"))
    p.check("metrics: every endpoint is registered with its host identity",
            len(start.get("endpoints", [])) > 0 and all("host" in e for e in start["endpoints"]),
            "endpoints=%r" % start.get("endpoints"))
    # At rest every lease has been returned, so the in-use gauge nets to zero
    in_use = metric(start, "slots_in_use")
    p.check("metrics: the in-use slot gauge is quiescent at rest", in_use == 0,
            "expected 0 in-use slots, got %d" % in_use)

    # Success path: requests, completions and latency samples each move by exactly the
    # number driven, and bytes flow in both directions
    driven = 20
    results = [mux_call(cfg) for _ in range(driven)]
    p.check("metrics: the driven calls all succeeded", all(is_ok(r) for r in results),
            "results=%r" % results)

    after = metrics(cfg)
    for field in ("requests", "completed"):
        moved = metric(after, field) - metric(start, field)
        p.check("metrics: %s matches the calls driven" % field, moved == driven,
                "expected +%d, got +%d" % (driven, moved))

    moved = latency_metric(after, "count") - latency_metric(start, "count")
    p.check("metrics: one latency sample per completion", moved == driven,
            "expected +%d, got +%d" % (driven, moved))
    p.check("metrics: bytes_out is recorded on send",
            metric(after, "bytes_out") - metric(start, "bytes_out") > 0)
    p.check("metrics: bytes_in is recorded on receive",
            metric(after, "bytes_in") - metric(start, "bytes_in") > 0)

    # Every failure has to land in one of these rather than being dropped on the floor
    buckets = ("connect_failures", "reconnects", "other_errors", "tls_failures")

    def bucket_deltas(baseline):
        snapshot = metrics(cfg)
        return {name: metric(snapshot, name) - baseline[name] for name in buckets}

    # A handshake the server refuses outright never becomes a served request
    baseline = {name: metric(metrics(cfg), name) for name in buckets}
    r = mux_call(cfg, mux="bad", rtimeout=18.0)
    moved = bucket_deltas(baseline)
    p.check("metrics: a refused handshake is never served", is_err(r), "r=%r" % r)
    p.check("metrics: a refused handshake lands in a failure bucket", sum(moved.values()) >= 1,
            "no failure bucket moved: %r" % moved)

    # A garbage ServerHello fails inside onConnect, where the waiting caller is not
    # attached to the slot, so the exact bucket depends on timing: an immediate
    # OpenSSL error with the caller attached lands in tls_failures, a stall trips the
    # connect timer into connect_failures, and a background retry shows as reconnects.
    # What must always hold is that it is recorded somewhere and never served
    baseline = {name: metric(metrics(cfg), name) for name in buckets}
    r = solo_get(cfg, key="x", solo="tlsgarbage", rtimeout=20.0)
    moved = bucket_deltas(baseline)
    p.check("metrics: a garbage TLS handshake is never served", is_err(r), "r=%r" % r)
    p.check("metrics: a failed TLS handshake lands in a failure bucket", sum(moved.values()) >= 1,
            "no failure bucket moved: %r" % moved)

    # Gauge invariant: every lease taken above has since been returned, which is what
    # catches an unbalanced increment and decrement
    end = metrics(cfg)
    p.check("metrics: the in-use slot gauge is back to zero after the phase",
            metric(end, "slots_in_use") == 0,
            "expected 0 in-use slots, got %d" % metric(end, "slots_in_use"))

    p.check("worker healthy after the metrics phase", wfx_healthy(cfg) and mock.ping())

class EndpointAudit(common.Suite):
    name = "endpoint_audit"
    description = "WFX Endpoint<> primitive audit"
    phases = {
        "mux_handshake":  phase_mux_handshake,
        "mux_multiplex":  phase_mux_multiplex,
        "mux_soak":       phase_mux_soak,
        "solo_pinning":   phase_solo_pinning,
        "solo_push":      phase_solo_push,
        "solo_streaming": phase_solo_streaming,
        "solo_upgrade":   phase_solo_upgrade,
        "solo_abort":     phase_solo_abort,
        "metrics":        phase_metrics,
    }

    def add_arguments(self, parser):
        parser.add_argument("--control-port", type=int, default=8091,
                            help="mock control-plane port, read by this suite only")
        parser.add_argument("--mux-port", type=int, default=8092,
                            help="mock mux port, MUST match MUX_UPSTREAM in app/src/main.cpp")
        parser.add_argument("--solo-port", type=int, default=8093,
                            help="mock solo port, MUST match SOLO_UPSTREAM in app/src/main.cpp")

    def configure(self, cfg):
        cfg.control_port = cfg.args.control_port
        cfg.mux_port = cfg.args.mux_port
        cfg.solo_port = cfg.args.solo_port

        # The two protocol ports are compiled into the app, so a mismatch means the
        # audit drives an endpoint that can never reach the mock
        for flag, port, default, source in (("--mux-port", cfg.mux_port, 8092, "MUX_UPSTREAM"),
                                            ("--solo-port", cfg.solo_port, 8093, "SOLO_UPSTREAM")):
            if port != default:
                term.log("runner", term.yellow(
                    "NOTE: %s=%d must match %s in app/src/main.cpp (default %d), the port is baked "
                    "in at compile time" % (flag, port, source, default)))

    def setup(self, ctx):
        ctx.resources["mock"] = Mock(ctx.cfg)
        ctx.mock.start()

    def before_phases(self, ctx):
        # Without this, every phase fails for the same uninformative reason
        if not is_ok(mux_call(ctx.cfg)):
            ctx.phase("preflight").failed(
                "WFX can reach the mock's mux listener",
                "is MUX_UPSTREAM in app/src/main.cpp pointing at port %d?" % ctx.cfg.mux_port)
            return False

    def teardown(self, ctx):
        if "mock" in ctx.resources:
            ctx.mock.stop()

if __name__ == "__main__":
    common.run(EndpointAudit)
