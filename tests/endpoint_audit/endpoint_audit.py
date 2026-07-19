#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX HttpEndpoint audit
#
# Boots a hostile mock upstream (upstream.py) and the WFX endpoint test app, then
# drives 150+ adversarial vectors THROUGH WFX at the mock and asserts the client-
# side parser/serializer in include/wfx/endpoint/http.hpp behaves: never crashes,
# never hangs past its timeout, never mis-frames one response into another, never
# smuggles a request, never lets a hostile upstream poison a pooled connection
#
# Architecture (mirrors tests/base_audit): the .py coordinates everything. The audit
# talks to WFX; WFX talks to the mock. For byte-level fuzzing the audit STAGES a
# raw response blob at the mock (/ctl/stage) then asks WFX to fetch /raw/<id>, so
# the mock replays those exact bytes. WFX reflects the parsed outcome back as JSON:
#   {"ep": <EndpointStatus int, 0==SUCCESS>, "status", "bodylen", "body", "hdr"}
#
# Exit codes:  0 all pass   1 correctness failure   2 SECURITY finding (desync,
#              smuggle, request-injection); these are called out separately

import argparse
import itertools
import json
import os
import subprocess
import sys
import threading
import time

# Suites are run directly, so tests/ has to be on the path before common is importable
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import common
from common import net, term

_green, _red, _yellow = term.green, term.red, term.yellow

# Mirrors EndpointStatus in shared/abis/types.hpp, keep in sync
EP_SUCCESS           = 0
EP_POOL_EXHAUSTED    = 6
EP_INTERNAL          = 10   # parse / protocol error surfaces here
EP_SERIALIZE         = 11
EP_HANDSHAKE_TIMEOUT = 13
EP_REQ_TIMEOUT       = 14

# Transport, short names because they are on nearly every line below
raw_send   = net.send
_build     = net.request
_body_of   = net.body
_status_of = net.status

# The mock upstream
class Mock:
    def __init__(self, cfg):
        self.cfg = cfg
        self.proc = None

    def start(self):
        script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "upstream.py")
        cmd = [sys.executable, script, "--host", self.cfg.host, "--port", str(self.cfg.up_port),
              "--proto-port", str(self.cfg.proto_port), "--sp-port", str(self.cfg.sp_port)]
        term.log("mock", "starting: %s" % " ".join(cmd))
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        for _ in range(50):
            if self.ping():
                term.log("mock", _green("up on %s:%d" % (self.cfg.host, self.cfg.up_port)))
                return
            time.sleep(0.1)
        raise RuntimeError("mock upstream never came up on port %d" % self.cfg.up_port)

    def ping(self):
        raw = raw_send(self.cfg.host, self.cfg.up_port, _build("GET", "/ctl/ping"), rtimeout=1.0, ctimeout=1.0)
        return _body_of(raw) == b"pong" if raw else False

    def get(self, path):
        raw = raw_send(self.cfg.host, self.cfg.up_port, _build("GET", path))
        return _body_of(raw) if raw else b""

    def stage(self, sid, blob, keep, mode="whole", arg=0):
        raw_send(self.cfg.host, self.cfg.up_port,
                 _build("POST", "/ctl/stage",
                        {"X-Id": sid, "X-Keep": "1" if keep else "0",
                         "X-Mode": mode, "X-Arg": str(arg)}, blob))

    def coalesce_reset(self):
        raw_send(self.cfg.host, self.cfg.up_port, _build("GET", "/ctl/coalesce/reset"))

    def coalesce_count(self):
        try:
            return int(self.get("/ctl/coalesce/count"))
        except ValueError:
            return -1

    def conn_count(self):
        try:
            return int(self.get("/ctl/conns"))
        except ValueError:
            return -1

    def proto_conn_count(self):
        try:
            return int(self.get("/ctl/protoconns"))
        except ValueError:
            return -1

    def total_count(self):
        try:
            return int(self.get("/ctl/total"))
        except ValueError:
            return -1

    def idle_conns(self):
        # Atomic (accepted conns - served requests): idle/prewarmed connections
        try:
            return int(self.get("/ctl/idleconns"))
        except ValueError:
            return -1

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

# Driving WFX
# Staged responses are keyed by a unique id so a replay can never pick up a previous one
_sid = itertools.count(1)
def _next_sid():
    return "s%d" % next(_sid)

def drive(cfg, path, ep="default", method="GET", want=None, fwd=None, body=None, x_body=None, rtimeout=8.0):
    """Hit WFX /call and return the parsed JSON dict (or None if WFX misbehaved).

    rtimeout must exceed the outbound budget for slow/connect-failure endpoints:
    WFX won't answer /call until the outbound request resolves (times out), which
    can take up to ~10s (5s budget + up to a 5s timer-tick of slack)."""
    headers = {"X-Ep": ep, "X-Method": method, "X-Path": path}
    if want:
        headers["X-Want"] = want
    if fwd:
        headers["X-Fwd"] = fwd
    if x_body is not None:
        headers["X-Body"] = x_body
    raw = raw_send(cfg.host, cfg.port, _build("GET", "/call", headers, body or b""), rtimeout=rtimeout)
    if not raw or _status_of(raw) != 200:
        return None
    try:
        return json.loads(_body_of(raw))
    except Exception:
        return None

# keep defaults to True so the mock keeps the connection open after replaying a
# COMPLETE self-delimiting (Content-Length / chunked) response, which is exactly
# what the client does (it pools the keep-alive slot). If the mock closed instead
# (keep=False) while the client pooled the slot, the NEXT staged request would
# reuse a dead socket and intermittently get None. EOF / truncation / close-
# delimited tests must pass keep=False explicitly, because they RELY on the close
def drive_staged(cfg, mock, blob, ep="default", keep=True, mode="whole", arg=0):
    sid = _next_sid()
    mock.stage(sid, blob, keep, mode, arg)
    return drive(cfg, "/raw/%s" % sid, ep=ep)

def drive_split(cfg, mock, blob, off=0, ep="default", keep=True):
    """Deliver `blob` as two sends split at byte `off` (0 => midpoint)."""
    return drive_staged(cfg, mock, blob, ep=ep, keep=keep, mode="split", arg=off)

def drive_drip(cfg, mock, blob, piece=1, ep="default", keep=True):
    """Deliver `blob` `piece` bytes at a time, one recv() boundary per piece."""
    return drive_staged(cfg, mock, blob, ep=ep, keep=keep, mode="drip", arg=piece)

def inject(cfg, mode, body, ep="default"):
    headers = {"X-Ep": ep, "X-Inject": mode}
    raw = raw_send(cfg.host, cfg.port, _build("POST", "/inject", headers, body))
    if not raw or _status_of(raw) != 200:
        return None
    try:
        return json.loads(_body_of(raw))
    except Exception:
        return None

# /proto/* helpers
# onConnect / onDisconnect / multiplexing, driven against app/src/proto.cpp's raw-
# -WFX::Endpoint<> instances (good/bad/slow/reset)
def proto_call(cfg, key="hello", proto="good", rtimeout=8.0):
    headers = {"X-Proto": proto, "X-Key": key}
    raw = raw_send(cfg.host, cfg.port, _build("GET", "/proto/call", headers), rtimeout=rtimeout)
    if not raw or _status_of(raw) != 200:
        return None
    try:
        return json.loads(_body_of(raw))
    except Exception:
        return None

def proto_call_async(cfg, key="hello", proto="good", rtimeout=8.0):
    """Fire proto_call on a background thread; returns a function that blocks for the result."""
    box = {}
    def run():
        box["r"] = proto_call(cfg, key=key, proto=proto, rtimeout=rtimeout)
    t = threading.Thread(target=run)
    t.start()
    def join():
        t.join()
        return box.get("r")
    return join

def proto_disconnects(cfg):
    raw = raw_send(cfg.host, cfg.port, _build("GET", "/proto/disconnects"))
    if not raw or _status_of(raw) != 200:
        return {"idle": -1, "handshake": -1, "error": -1}
    try:
        return json.loads(_body_of(raw))
    except Exception:
        return {"idle": -1, "handshake": -1, "error": -1}

def proto_disconnects_reset(cfg):
    raw_send(cfg.host, cfg.port, _build("POST", "/proto/disconnects/reset"))

# Expectation predicates over the parsed result dict
def is_ok(r, status=None, body=None):
    if not r or r.get("ep") != EP_SUCCESS:
        return False
    if status is not None and r.get("status") != status:
        return False
    if body is not None and r.get("body") != body:
        return False
    return True

def is_err(r):          # WFX answered, but the outbound call failed cleanly
    return bool(r) and r.get("ep") != EP_SUCCESS

def is_errc(r, code):
    return bool(r) and r.get("ep") == code

def is_ok_sp(r):        # SP routes report ep plus value/conn, no HTTP status
    return bool(r) and r.get("ep") == EP_SUCCESS

def is_alive(r):        # WFX answered *something* well-formed (no crash/hang)
    return r is not None

# /reflectraw helpers: inspect the EXACT request head WFX emitted
def raw_head(cfg, fwd=None, fwd2=None, fwd3=None, method="GET", x_body=None):
    """Drive /reflectraw and return the request head string WFX put on the wire."""
    headers = {"X-Ep": "default", "X-Method": method, "X-Path": "/reflectraw"}
    if fwd:
        headers["X-Fwd"]  = fwd
    if fwd2:
        headers["X-Fwd2"] = fwd2
    if fwd3:
        headers["X-Fwd3"] = fwd3
    if x_body is not None:
        headers["X-Body"] = x_body
    raw = raw_send(cfg.host, cfg.port, _build("GET", "/call", headers, b""))
    if not raw or _status_of(raw) != 200:
        return None
    try:
        return json.loads(_body_of(raw)).get("body")
    except Exception:
        return None

def _count_ci(head, needle):
    return head.lower().count(needle.lower())

# SP protocol helpers (pinning / streaming / push / TLS upgrade)
# SP is the non-multiplexed protocol on the third mock listener. Proto sets
# hasCapacity so it is permanently multiplexed, and pinning + streaming are
# single-slot-only paths, so none of this is reachable through Proto
def sp_json(cfg, path, headers=None, rtimeout=15.0, method="GET"):
    raw = raw_send(cfg.host, cfg.port, _build(method, path, headers or {}), rtimeout=rtimeout)
    if not raw or _status_of(raw) != 200:
        return None
    try:
        return json.loads(_body_of(raw))
    except Exception:
        return None

def sp_get(cfg, key="hello", sp="good", rtimeout=15.0):
    return sp_json(cfg, "/sp/get", {"X-Sp": sp, "X-Key": key}, rtimeout)

def sp_stream(cfg, count=10, size=64, mode="server", stop=0, rtimeout=60.0):
    return sp_json(cfg, "/sp/stream", {"X-Mode": mode, "X-Count": str(count),
                                       "X-Size": str(size), "X-Stop": str(stop)}, rtimeout)

def sp_reserve(cfg, n=3, release="late", rtimeout=20.0):
    return sp_json(cfg, "/sp/reserve", {"X-N": str(n), "X-Release": release}, rtimeout)

def sp_push_stats(cfg):
    return sp_json(cfg, "/sp/push") or {}

def sp_push_reset(cfg):
    raw_send(cfg.host, cfg.port, _build("POST", "/sp/push/reset"))

def sp_rss(cfg):
    return sp_json(cfg, "/sp/rss") or {}

def sp_async(fn, *a, **kw):
    """Run any sp_* helper on a thread; returns a joiner."""
    box = {}
    def run():
        box["r"] = fn(*a, **kw)
    t = threading.Thread(target=run)
    t.start()
    def join():
        t.join()
        return box.get("r")
    return join

# Wire builders for staged responses
def R(status_line, headers=(), body=b""):
    if isinstance(body, str):
        body = body.encode("latin-1")
    head = status_line + "\r\n" + "".join("%s\r\n" % h for h in headers) + "\r\n"
    return head.encode("latin-1") + body

def CHUNKED(chunks_raw):
    return b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunks_raw

# PHASE 1: happy-path framing (the client must accept the whole legal matrix)
def phase_framing(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("framing")
    p.check("content-length small",       is_ok(drive(cfg, "/ok"), 200, "hello"))
    p.check("content-length zero",        is_ok(drive(cfg, "/empty"), 200, ""))
    p.check("content-length 1000",        (lambda r: is_ok(r, 200) and r.get("bodylen") == 1000)(drive(cfg, "/cl/1000")))
    p.check("chunked single",             is_ok(drive(cfg, "/chunked/1"), 200, "[c0]"))
    p.check("chunked multi (5)",          is_ok(drive(cfg, "/chunked/5"), 200, "[c0][c1][c2][c3][c4]"))
    p.check("chunked extension ignored",  is_ok(drive(cfg, "/chunked-ext"), 200, "abc"))
    p.check("chunked trailer discarded",  is_ok(drive(cfg, "/chunked-trailer"), 200, "xy"))
    p.check("close-delimited body",       is_ok(drive(cfg, "/close"), 200, "closebody"))
    p.check("http/1.0 close-delimited",   is_ok(drive(cfg, "/http10"), 200, "ten"))
    p.check("HEAD is bodyless",           is_ok(drive(cfg, "/evil/headbody", method="HEAD"), 200, "") or
                                            is_ok(drive(cfg, "/kacount", method="HEAD"), 200, ""))
    p.check("1xx: 100 then 200",          is_ok(drive(cfg, "/continue"), 200, "after"))
    p.check("1xx: 8 informational ok",    is_ok(drive(cfg, "/continue/8"), 200, "done"))
    p.check("response header retrievable", (lambda r: is_ok(r, 200) and r.get("hdr") == "alpha")(drive(cfg, "/ok", want="X-Mark")))
    p.check("header get case-insensitive", (lambda r: is_ok(r, 200) and r.get("hdr") == "alpha")(drive(cfg, "/ok", want="x-mark")))
    for code in (200, 201, 202, 301, 400, 404, 418, 500, 599, 999):
        p.check("status passthrough %d" % code, is_ok(drive(cfg, "/status/%d" % code), code))
    p.check("status 204 no body",         is_ok(drive(cfg, "/status/204"), 204, ""))
    p.check("status 304 no body",         is_ok(drive(cfg, "/status/304"), 304, ""))

# PHASE 2: status-line fuzz (malformed first line must be rejected, not crash)
def phase_statusline(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("statusline")

    # (name, status_line, expectation), response is <line>\r\nContent-Length: 0\r\n\r\n
    ok_lines = [
        ("valid 200",          "HTTP/1.1 200 OK"),
        ("valid no reason",    "HTTP/1.1 200"),
        ("valid 999",          "HTTP/1.1 999 X"),
        ("valid 000",          "HTTP/1.1 000 X"),
        ("http/1.0",           "HTTP/1.0 200 OK"),
        ("valid 042 leadzero", "HTTP/1.1 042 X"),
        ("reason with digits", "HTTP/1.1 200 200 OK"),
        ("no-space reason",    "HTTP/1.1 200OK"),      # first 12 chars valid -> accepted
        ("long reason",        "HTTP/1.1 200 " + "R" * 300),
        ("reason w/ colon",    "HTTP/1.1 200 O:K"),
        ("reason w/ tab-in",   "HTTP/1.1 200 O\tK"),
        ("tab after code",     "HTTP/1.1 200\tOK"),    # separator before reason not validated -> accepted
    ]
    for name, line in ok_lines:
        r = drive_staged(cfg, mock, (line + "\r\nContent-Length: 0\r\n\r\n").encode("latin-1"))
        p.check("line ok: %s" % name, is_ok(r))

    bad_lines = [
        ("http/2.0",           "HTTP/2.0 200 OK"),
        ("http/3.0",           "HTTP/3.0 200 OK"),
        ("http/1.2 minor",     "HTTP/1.2 200 OK"),
        ("http/0.9",           "HTTP/0.9 200 OK"),
        ("http/1.9 minor",     "HTTP/1.9 200 OK"),
        ("major non-digit",    "HTTP/x.1 200 OK"),
        ("lowercase proto",    "http/1.1 200 OK"),
        ("mixed-case proto",   "HTTp/1.1 200 OK"),
        ("comma version",      "HTTP/1,1 200 OK"),
        ("colon version",      "HTTP/1:1 200 OK"),
        ("no dot version",     "HTTP/11 200 OK"),
        ("underscore version", "HTTP/1_1 200 OK"),
        ("dot shifted",        "HTTP/1.10 200 OK"),
        ("backslash proto",    "HTTP\\1.1 200 OK"),
        ("ICY (shoutcast)",    "ICY 200 OK"),
        ("RTSP proto",         "RTSP/1.0 200 OK"),
        ("leading space",      " HTTP/1.1 200 OK"),
        ("prefix junk",        "XHTTP/1.1 200 OK"),
        ("short line",         "HTTP/1.1 20"),
        ("bare proto",         "HTTP/1.1"),
        ("empty line",         ""),
        ("2-digit code",       "HTTP/1.1 99 X"),
        ("1-digit code",       "HTTP/1.1 2 X"),
        ("alpha code",         "HTTP/1.1 abc X"),
        ("plus code",          "HTTP/1.1 +20 X"),
        ("minus code",         "HTTP/1.1 -20 X"),
        ("spaced code",        "HTTP/1.1 20 0 X"),
        ("double space",      r"HTTP/1.1  200 X"),
        ("tab separator",      "HTTP/1.1\t200 X"),
        ("no space after ver", "HTTP/1.1_200 X"),
        ("http/3.0",           "HTTP/3.0 200 OK"),
        ("http/1.11 minor",    "HTTP/1.11 200 OK"),
        ("http/10.1 major",    "HTTP/10.1 200 OK"),
        ("empty version",      "HTTP/. 200 OK"),
        ("space in version",   "HTTP/1 .1 200 OK"),
        ("trailing dot",       "HTTP/1. 200 OK"),
        ("hex code 0x",        "HTTP/1.1 0x0 X"),
        ("code with space",    "HTTP/1.1 2 0 0 X"),
        ("nul in code",        "HTTP/1.1 2\x000 X"),
        ("cr only line",       "HTTP/1.1 200 OK\r"),   # trailing \r stripped -> 'OK\r' becomes 'OK'? still valid; see below
        ("just HTTP/",         "HTTP/"),
        ("HTTP no slash",      "HTTP1.1 200 OK"),
        ("SIP proto",          "SIP/2.0 200 OK"),
        ("gopher junk",        "gopher://x 200"),
        ("leading tab",        "\tHTTP/1.1 200 OK"),
        ("double proto",       "HTTP/1.1 HTTP/1.1 200"),
        ("code then letters",  "HTTP/1.1 20a X"),
        ("negative version",   "HTTP/-1.1 200 OK"),
    ]
    for name, line in bad_lines:
        # "cr only line" actually parses valid (trailing CR is stripped), so skip its err-assertion
        if name == "cr only line":
            r = drive_staged(cfg, mock, (line + "\nContent-Length: 0\r\n\r\n").encode("latin-1"))
            p.check("line edge: %s" % name, is_alive(r))
            continue
        r = drive_staged(cfg, mock, (line + "\r\n\r\n").encode("latin-1"))
        p.check("line bad: %s" % name, is_err(r), "expected err, got %r" % r)

# PHASE 3: header fuzz (malformed / smuggling headers)
def phase_headers(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("headers")

    def with_hdr(hdr_block, body=b"hi", cl=None):
        cl = len(body) if cl is None else cl
        return ("HTTP/1.1 200 OK\r\n%s\r\nContent-Length: %d\r\n\r\n" % (hdr_block, cl)).encode("latin-1") + body

    # Accepted header shapes
    p.check("empty value ok",        is_ok(drive_staged(cfg, mock, with_hdr("X-Empty:")), 200, "hi"))
    p.check("value OWS trimmed",     (lambda r: is_ok(r, 200))(drive_staged(cfg, mock, with_hdr("X-A:    v   "))))
    p.check("dup CL equal ok",       is_ok(drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nhi"), 200, "hi"))
    p.check("CL leading zeros",      is_ok(drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nContent-Length: 0002\r\n\r\nhi"), 200, "hi"))
    p.check("Connection: close ok",  is_ok(drive_staged(cfg, mock, with_hdr("Connection: close")), 200, "hi"))
    p.check("Connection keep,close", is_ok(drive_staged(cfg, mock, with_hdr("Connection: keep-alive, close")), 200, "hi"))
    p.check("Connection Close case", is_ok(drive_staged(cfg, mock, with_hdr("Connection: Close")), 200, "hi"))
    p.check("Connection close,keep", is_ok(drive_staged(cfg, mock, with_hdr("Connection: close, keep-alive")), 200, "hi"))
    p.check("Connection tabs tokens",is_ok(drive_staged(cfg, mock, with_hdr("Connection: \tkeep-alive\t,\tclose\t")), 200, "hi"))
    p.check("Connection closed!=close",is_ok(drive_staged(cfg, mock, with_hdr("Connection: closed")), 200, "hi"))
    p.check("value many colons ok",  (lambda r: is_ok(r, 200))(drive_staged(cfg, mock, with_hdr("X-A: a:b:c:d"))))
    p.check("value all-spaces empty", is_ok(drive_staged(cfg, mock, with_hdr("X-A:      ")), 200, "hi"))
    p.check("value tab-trimmed",     is_ok(drive_staged(cfg, mock, with_hdr("X-A:\tv\t")), 200, "hi"))
    p.check("dup CL triple equal",   is_ok(drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nhi"), 200, "hi"))
    p.check("CL trailing OWS ok",    is_ok(drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nContent-Length: 2 \r\n\r\nhi"), 200, "hi"))
    p.check("many benign headers",   is_ok(drive_staged(cfg, mock, ("HTTP/1.1 200 OK\r\n" + "".join("X-H%d: v%d\r\n" % (i, i) for i in range(30)) + "Content-Length: 2\r\n\r\nhi").encode("latin-1")), 200, "hi"))
    p.check("dup TE chunked ok",     is_ok(drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"), 200, "abc"))

    # Rejected header shapes (framing / smuggling)
    bad = [
        ("obs-fold continuation", b"HTTP/1.1 200 OK\r\nX-Fold: a\r\n b\r\nContent-Length: 2\r\n\r\nhi"),
        ("leading-tab fold",      b"HTTP/1.1 200 OK\r\n\tX: y\r\nContent-Length: 2\r\n\r\nhi"),
        ("no colon",              with_hdr("NoColonHeader")),
        ("empty name",            with_hdr(": value")),
        ("space before colon",    with_hdr("Name : value")),
        ("tab before colon",      with_hdr("Name\t: value")),
        ("dup CL differ",         b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nhi"),
        ("CL then TE",            b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\nhi"),
        ("TE then CL",            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\nhi"),
        ("CL non-numeric",        with_hdr("Content-Length: 2x", cl=None)),
        ("CL hex",                b"HTTP/1.1 200 OK\r\nContent-Length: 0x2\r\n\r\nhi"),
        ("CL plus",               b"HTTP/1.1 200 OK\r\nContent-Length: +2\r\n\r\nhi"),
        ("CL minus",              b"HTTP/1.1 200 OK\r\nContent-Length: -2\r\n\r\nhi"),
        ("CL internal space",     b"HTTP/1.1 200 OK\r\nContent-Length: 2 2\r\n\r\nhi"),
        ("CL empty",              b"HTTP/1.1 200 OK\r\nContent-Length:\r\n\r\nhi"),
        ("CL overflow u64",       b"HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999999\r\n\r\nhi"),
        ("TE gzip",               b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nx"),
        ("TE chunked,gzip",       b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\nx"),
        ("TE gzip,chunked",       b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\nx"),
        ("TE x-chunked",          b"HTTP/1.1 200 OK\r\nTransfer-Encoding: xchunked\r\n\r\nx"),
        ("TE identity",           b"HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\n\r\nx"),
        ("TE chunked;q=0",        b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;q=0\r\n\r\nx"),
        ("TE Chunked,chunked",    b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, chunked\r\n\r\nx"),
        ("TE deflate",            b"HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate\r\n\r\nx"),
        ("TE then CL differ",     b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\nhi"),
        ("CL then TE case",       b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\ntransfer-encoding: CHUNKED\r\n\r\nhi"),
        ("dup CL differ 2vs0",    b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 0\r\n\r\nhi"),
        ("CL with plus-space",    b"HTTP/1.1 200 OK\r\nContent-Length: + 2\r\n\r\nhi"),
        ("CL float",              b"HTTP/1.1 200 OK\r\nContent-Length: 2.0\r\n\r\nhi"),
        ("CL binary 0b10",        b"HTTP/1.1 200 OK\r\nContent-Length: 0b10\r\n\r\nhi"),
        ("CL comma sep",          b"HTTP/1.1 200 OK\r\nContent-Length: 2,2\r\n\r\nhi"),
        ("CL leading space only", b"HTTP/1.1 200 OK\r\nContent-Length:    \r\n\r\nhi"),
        ("blank-ish fold line",   b"HTTP/1.1 200 OK\r\n \r\nContent-Length: 2\r\n\r\nhi"),
        ("only colon header",     b"HTTP/1.1 200 OK\r\n:\r\nContent-Length: 2\r\n\r\nhi"),
        ("double colon empty nm", b"HTTP/1.1 200 OK\r\n::v\r\nContent-Length: 2\r\n\r\nhi"),
    ]
    for name, blob in bad:
        r = drive_staged(cfg, mock, blob)
        sec = name in ("obs-fold continuation", "space before colon", "CL then TE", "TE then CL",
                       "dup CL differ", "TE chunked,gzip", "TE gzip,chunked", "TE then CL differ",
                       "CL then TE case", "dup CL differ 2vs0", "TE Chunked,chunked", "TE chunked;q=0",
                       "blank-ish fold line")
        p.check("hdr bad: %s" % name, is_err(r), "expected err, got %r" % r, security=sec)

    # Accepted TE spellings (need a real chunked body)
    for name, te in [("TE Chunked case", "Chunked"), ("TE CHUNKED case", "CHUNKED"),
                     ("TE leading space", " chunked"), ("TE trailing space", "chunked "),
                     ("TE tab-wrapped", "\tchunked\t"), ("TE cHuNkEd", "cHuNkEd"),
                     ("TE spaces both", "   chunked   ")]:
        blob = ("HTTP/1.1 200 OK\r\nTransfer-Encoding: %s\r\n\r\n3\r\nabc\r\n0\r\n\r\n" % te).encode("latin-1")
        p.check("hdr ok: %s" % name, is_ok(drive_staged(cfg, mock, blob), 200, "abc"))

# PHASE 4: chunked-body fuzz
def phase_chunked(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("chunked")

    ok = [
        ("upper hex size",   b"A\r\n0123456789\r\n0\r\n\r\n", "0123456789"),
        ("lower hex size",   b"a\r\n0123456789\r\n0\r\n\r\n", "0123456789"),
        ("leading zero size",b"003\r\nabc\r\n0\r\n\r\n", "abc"),
        ("ext quoted",       b"3;a=\"b\"\r\nabc\r\n0\r\n\r\n", "abc"),
        ("zero-only body",   b"0\r\n\r\n", ""),
        ("many small chunks",b"1\r\na\r\n1\r\nb\r\n1\r\nc\r\n0\r\n\r\n", "abc"),
        ("trailer then end", b"2\r\nxy\r\n0\r\nX-T: v\r\nY-T: w\r\n\r\n", "xy"),
        ("long leadzero size",b"000000000003\r\nabc\r\n0\r\n\r\n", "abc"),
        ("multi ext",        b"3;a=1;b=2\r\nabc\r\n0\r\n\r\n", "abc"),
        ("ext no value",     b"3;flag\r\nabc\r\n0\r\n\r\n", "abc"),
        ("uppercase hexdigs",b"F\r\n0123456789ABCDE\r\n0\r\n\r\n", "0123456789ABCDE"),
        ("mixedcase size",   b"aB\r\n" + b"Z" * 0xAB + b"\r\n0\r\n\r\n", "Z" * 0xAB),
        ("0-size w/ ext",    b"0;done\r\n\r\n", ""),
        ("0-size w/ trailer",b"0\r\nX-Only-Trailer: yes\r\n\r\n", ""),
        ("crlf-heavy tiny",  b"1\r\nx\r\n1\r\ny\r\n1\r\nz\r\n1\r\nw\r\n0\r\n\r\n", "xyzw"),
    ]
    for name, chunks, expect in ok:
        p.check("chunk ok: %s" % name, is_ok(drive_staged(cfg, mock, CHUNKED(chunks)), 200, expect))

    # 200 tiny 1-byte chunks must reassemble to exactly 200 bytes
    many = b"".join(b"1\r\n%c\r\n" % (0x61 + (i % 26)) for i in range(200)) + b"0\r\n\r\n"
    p.check("chunk 200x1-byte reassembly", (lambda r: is_ok(r, 200) and r.get("bodylen") == 200)(
        drive_staged(cfg, mock, CHUNKED(many))))

    bad = [
        ("non-hex size",      b"zz\r\nabc\r\n0\r\n\r\n"),
        ("partial-hex size",  b"1g\r\nabc\r\n0\r\n\r\n"),
        ("0x prefix size",    b"0x3\r\nabc\r\n0\r\n\r\n"),
        ("empty size ;ext",   b";ext\r\nabc\r\n0\r\n\r\n"),
        ("size overflow",     b"FFFFFFFFFFFFFFFFFF\r\nabc\r\n0\r\n\r\n"),
        ("negative size",     b"-1\r\nabc\r\n0\r\n\r\n"),
        ("space in size",     b"3 \r\nabc\r\n0\r\n\r\n"),
        ("tab in size",       b"3\t\r\nabc\r\n0\r\n\r\n"),
        ("bad chunk terminator", b"3\r\nabcX5\r\n0\r\n\r\n"),
        ("data shorter+eof",  b"5\r\nab"),
        ("empty size line",   b"\r\nabc\r\n0\r\n\r\n"),
        ("size plus sign",    b"+3\r\nabc\r\n0\r\n\r\n"),
        ("size 0x prefix up", b"0X3\r\nabc\r\n0\r\n\r\n"),
        ("size w/ inner space",b"3 0\r\nabc\r\n0\r\n\r\n"),
        ("size hash junk",    b"3#\r\nabc\r\n0\r\n\r\n"),
        ("size u64+1 overflow",b"10000000000000000\r\nabc\r\n0\r\n\r\n"),
        ("term one byte off", b"3\r\nabcX\r\n0\r\n\r\n"),
        ("term missing lf",   b"3\r\nabc\r0\r\n\r\n"),
        ("data over size+eof",b"2\r\nabcdef"),
        ("neg then valid",    b"-0\r\n\r\n"),
        ("size dot",          b"3.\r\nabc\r\n0\r\n\r\n"),
    ]
    for name, chunks in bad:
        sec = name in ("term one byte off", "bad chunk terminator", "term missing lf",
                       "size w/ inner space", "data over size+eof")
        # The "+eof" cases need the peer to CLOSE to signal truncation; the rest
        # error immediately on parse, so keep-alive vs close is irrelevant there
        p.check("chunk bad: %s" % name, is_err(drive_staged(cfg, mock, CHUNKED(chunks),
                                                              keep=not name.endswith("+eof"))),
              "expected err", security=sec)

    # 4 GiB single chunk: valid hex, but must be rejected against the body cap
    p.check("chunk 4GiB > cap", is_err(drive_staged(cfg, mock, CHUNKED(b"FFFFFFFF\r\n"))))

# PHASE 5: EOF / truncation at every parser phase (client must error, not hang)
def phase_eof(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("eof")
    trunc = [
        ("partial status line",   b"HTTP/1.1 200"),
        ("status line only",      b"HTTP/1.1 200 OK\r\n"),
        ("mid headers no blank",  b"HTTP/1.1 200 OK\r\nX: y\r\n"),
        ("headers then CL nobody",b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n"),
        ("mid CL body",           b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"),
        ("mid chunk size",        b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3"),
        ("chunk size no data",    b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\n"),
        ("mid chunk data",        b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab"),
        ("chunk trailer no end",  b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nxy\r\n0\r\n"),
    ]
    for name, blob in trunc:
        p.check("eof err: %s" % name, is_err(drive_staged(cfg, mock, blob, keep=False)), "expected err")

    # Legit EOF-delimited successes
    p.check("eof close-delimited ok", is_ok(drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\n\r\nbodybytes", keep=False), 200, "bodybytes"))
    p.check("eof empty body ok",      is_ok(drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", keep=False), 200, ""))
    p.check("zero-byte response",     is_err(drive(cfg, "/drop")))
    p.check("connection reset (RST)", is_err(drive(cfg, "/reset")))
    # Huge declared CL, tiny body, then close -> reserve path then EOF error
    p.check("huge CL then eof",       is_err(drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nContent-Length: 5000000\r\n\r\ntiny", keep=False)))

    # Same truncations, but delivered a byte at a time so the incremental parser
    # sits in each phase across many recv() boundaries before hitting EOF. Must
    # still error cleanly, never hang, never spin
    drip_trunc = [
        ("drip mid status",       b"HTTP/1.1 200"),
        ("drip mid headers",      b"HTTP/1.1 200 OK\r\nX: y\r\n"),
        ("drip CL nobody",        b"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nabc"),
        ("drip mid chunk size",   b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3"),
        ("drip mid chunk data",   b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nab"),
    ]
    for name, blob in drip_trunc:
        p.check("eof %s" % name, is_err(drive_drip(cfg, mock, blob, piece=1, keep=False)), "expected err")

# PHASE 6: DESYNC / SMUGGLING / keep-alive poisoning  (SECURITY)
def _reuse_clean(cfg, ep, n=10):
    """After a poison request on `ep`, every following /ok must be pristine."""
    for _ in range(n):
        if not is_ok(drive(cfg, "/ok", ep=ep), 200, "hello"):
            return False
    return True

def phase_desync(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("desync")

    # Each poison uses the small pool (connLimit 2) then hammers /ok on it: if the
    # hostile body poisoned a pooled connection, an /ok will misframe or return the
    # smuggled bytes. All of these are SECURITY findings if they trip
    scenarios = [
        ("204 with body",        lambda: drive(cfg, "/evil/204body", ep="small")),
        ("304 with body",        lambda: drive(cfg, "/evil/304body", ep="small")),
        ("HEAD with body",       lambda: drive(cfg, "/evil/headbody", ep="small", method="HEAD")),
        ("trailing smuggle",     lambda: drive(cfg, "/evil/trailing", ep="small")),
        ("pipelined 2 responses",lambda: drive(cfg, "/evil/pipeline", ep="small")),
    ]
    for name, poison in scenarios:
        poison()                       # fire the hostile response
        clean = _reuse_clean(cfg, "small", n=12)
        p.check("no poison after: %s" % name, clean,
              "pooled connection corrupted after %s" % name, security=True)

    # The smuggled body must never surface as a delivered response body
    r = drive(cfg, "/evil/trailing", ep="default")
    p.check("trailing body not smuggled", (r is None) or (r.get("body") != "SMUGGLE"),
          "client delivered smuggled bytes: %r" % r, security=True)

    # A legit 204/keep-alive connection stays reusable
    drive(cfg, "/status/204", ep="small")
    p.check("legit 204 keeps conn clean", _reuse_clean(cfg, "small"))

# PHASE 7: serialize / request-side (dedup + injection)  (injection is SECURITY)
def phase_serialize(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("serialize")
    host = "%s:%d" % (cfg.host, cfg.up_port) if cfg.up_port != 80 else cfg.host

    def reflect(fwd=None, method="GET", x_body=None):
        return drive(cfg, "/reflect", want=None, fwd=fwd, method=method, x_body=x_body)

    # Engine owns Host / Content-Length / Transfer-Encoding: caller dupes dropped
    for name, forged in [("Host lower", "host: evil.example"),
                         ("Host upper", "HOST: evil.example"),
                         ("Host mixed", "hOsT: evil.example")]:
        r = reflect(fwd=forged)
        ok = is_ok(r, 200) and ("host=%s|" % host) in (r.get("body") or "")
        p.check("dedup %s" % name, ok, "reflected: %r" % (r and r.get("body")), security=True)

    for name, forged in [("CL forge", "Content-Length: 999"), ("CL forge case", "content-length: 999")]:
        r = reflect(fwd=forged, method="POST", x_body="1234")
        ok = is_ok(r, 200) and "|clen=4|" in (r.get("body") or "")
        p.check("dedup %s" % name, ok, "reflected: %r" % (r and r.get("body")), security=True)

    for name, forged in [("TE forge", "Transfer-Encoding: chunked"), ("TE forge case", "transfer-encoding: chunked")]:
        r = reflect(fwd=forged)
        ok = is_ok(r, 200) and "|te=-|" in (r.get("body") or "")
        p.check("dedup %s" % name, ok, "reflected: %r" % (r and r.get("body")), security=True)

    # Normal header is forwarded verbatim
    r = reflect(fwd="X-Test: hello")
    p.check("forward normal header", is_ok(r, 200) and "|xtest=hello" in (r.get("body") or ""))

    # Content-Length correctness for a range of body sizes
    for n in (0, 1, 100, 1000):
        body = "z" * n
        r = drive(cfg, "/reflect", method="POST", x_body=body)
        ok = is_ok(r, 200) and ("|clen=%d|" % n) in (r.get("body") or "") and ("|blen=%d|" % n) in (r.get("body") or "")
        p.check("CL correct POST %d" % n, ok, "reflected: %r" % (r and r.get("body")))

    # GET with no body: no Content-Length emitted at all
    r = drive(cfg, "/reflect", method="GET")
    p.check("GET emits no CL", is_ok(r, 200) and "|clen=-|" in (r.get("body") or ""))

    # Large forwarded header forces serialize() buffer-grow retry; must still arrive
    big = "P" * 6000
    r = drive(cfg, "/reflect", fwd="X-Test: " + big)
    p.check("buffer-grow big header", is_ok(r, 200) and ("|xtest=" + big) in (r.get("body") or ""))

    # Request injection: hostile path / header bytes MUST be refused
    path_vectors = [
        ("CRLF in path",  b"/a\r\nX-Smuggle: 1"),
        ("bare LF path",  b"/a\nX-Smuggle: 1"),
        ("bare CR path",  b"/a\rX-Smuggle: 1"),
        ("NUL in path",   b"/a\x00b"),
        ("CRLF at end",   b"/ok\r\n"),
    ]
    for name, payload in path_vectors:
        r = inject(cfg, "path", payload)
        p.check("reject path: %s" % name, is_errc(r, EP_SERIALIZE), "expected serialize-err, got %r" % r, security=True)

    hdr_vectors = [
        ("CRLF in value", b"X-Evil: a\r\nX-Smuggle: b"),
        ("bare LF value", b"X-Evil: a\nb"),
        ("CRLF in name",  b"X-Evil\r\nX: b"),
        ("NUL in value",  b"X-Evil: a\x00b"),
    ]
    for name, payload in hdr_vectors:
        r = inject(cfg, "header", payload)
        p.check("reject header: %s" % name, is_errc(r, EP_SERIALIZE), "expected serialize-err, got %r" % r, security=True)

    # Injection controls: clean path / header succeed
    p.check("clean path accepted",   is_ok(inject(cfg, "path", b"/ok"), 200, "hello"))
    p.check("clean header accepted", is_ok(inject(cfg, "header", b"X-Ok: fine"), 200, "hello"))
    # Space in path isn't an injection (no CR/LF/NUL): must not crash, no smuggle
    p.check("space in path survives", is_alive(inject(cfg, "path", b"/a b")))

    # Byte-exact serialization via /reflectraw ('|' == CR/LF on the wire)
    hoststr = "%s:%d" % (cfg.host, cfg.up_port)
    h = raw_head(cfg)
    p.check("raw GET request line", bool(h) and h.startswith("GET /reflectraw HTTP/1.1|"), "head=%r" % h)
    p.check("raw Host exactly once", bool(h) and _count_ci(h, "|host:") == 1 and hoststr in h, "head=%r" % h)
    p.check("raw GET emits no CL",  bool(h) and _count_ci(h, "content-length") == 0, "head=%r" % h)

    # Forged Host buried between two clean headers -> dropped; clean ones kept, in order
    h = raw_head(cfg, fwd="X-One: 1", fwd2="Host: evil.example", fwd3="X-Two: 2")
    ok = (bool(h) and _count_ci(h, "|host:") == 1 and "evil.example" not in h and
          "|X-One: 1|" in h and "|X-Two: 2|" in h and h.index("X-One") < h.index("X-Two"))
    p.check("raw forged Host dropped+order", ok, "head=%r" % h, security=True)

    # Forged Content-Length / Transfer-Encoding dropped; engine's own CL is correct
    h = raw_head(cfg, fwd="Content-Length: 999", fwd2="Transfer-Encoding: chunked", method="POST", x_body="abcd")
    ok = (bool(h) and _count_ci(h, "content-length:") == 1 and "|Content-Length: 4|" in h and
          _count_ci(h, "transfer-encoding") == 0 and "999" not in h)
    p.check("raw forged CL/TE dropped", ok, "head=%r" % h, security=True)

    # Three clean forwarded headers preserved verbatim and in submission order
    h = raw_head(cfg, fwd="A: 1", fwd2="B: 2", fwd3="C: 3")
    p.check("raw three headers ordered", bool(h) and "|A: 1|B: 2|C: 3|" in h, "head=%r" % h)

    # Host header emitted immediately after the request line (before any forwarded header)
    h = raw_head(cfg, fwd="Z-First: z")
    p.check("raw Host precedes fwd headers", bool(h) and h.index("|Host:") < h.index("Z-First"), "head=%r" % h)

    # PUT/PATCH/POST with an EMPTY body still emit Content-Length: 0
    for m in ("PUT", "PATCH", "POST"):
        r = drive(cfg, "/reflect", method=m)
        p.check("%s empty emits CL0" % m, is_ok(r, 200) and "|clen=0|" in (r.get("body") or ""),
              "reflected %r" % (r and r.get("body")))

    # Large POST body forces a serialize() buffer-grow on the BODY, not just the header
    bigbody = "D" * 8000
    r = drive(cfg, "/reflect", method="POST", x_body=bigbody)
    p.check("buffer-grow big body", is_ok(r, 200) and ("|clen=%d|" % len(bigbody)) in (r.get("body") or "") and
          ("|blen=%d|" % len(bigbody)) in (r.get("body") or ""), "reflected clen/blen mismatch")

# PHASE 8: limit enforcement (boundaries on the small-caps endpoint)
def phase_limits(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("limits")
    # EpSmall: maxHeaderBytes=256, maxBodyBytes=1024, maxHeaderCount=8
    def hdr_block_bytes(total):
        # Build a header block roughly `total` bytes long via one padded header
        pad = max(0, total - len("X-P: \r\n"))
        return ("HTTP/1.1 200 OK\r\nX-P: %s\r\nContent-Length: 0\r\n\r\n" % ("A" * pad)).encode("latin-1")

    p.check("header block under cap", is_ok(drive_staged(cfg, mock, hdr_block_bytes(200), ep="small")))
    p.check("header block over cap",  is_err(drive_staged(cfg, mock, hdr_block_bytes(400), ep="small")))
    p.check("single huge header line",is_err(drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nX-P: " + b"A" * 600 + b"\r\nContent-Length: 0\r\n\r\n", ep="small")))

    def n_headers(n):
        block = "".join("X-H%d: v\r\n" % i for i in range(n))
        return ("HTTP/1.1 200 OK\r\n%sContent-Length: 0\r\n\r\n" % block).encode("latin-1")

    p.check("header count at cap (8)", is_ok(drive_staged(cfg, mock, n_headers(7), ep="small")))
    p.check("header count 8th ok",     is_ok(drive_staged(cfg, mock, n_headers(7), ep="small")))  # 7 + CL == 8
    p.check("header count 9th over",   is_err(drive_staged(cfg, mock, n_headers(8), ep="small")))  # 8 + CL == 9
    p.check("header count over cap",   is_err(drive_staged(cfg, mock, n_headers(20), ep="small")))

    # Exact byte boundary on maxBodyBytes for close-delimited + chunked framings
    p.check("chunk single at cap 1024",  (lambda r: is_ok(r, 200) and r.get("bodylen") == 1024)(
        drive_staged(cfg, mock, CHUNKED(b"400\r\n" + b"B" * 0x400 + b"\r\n0\r\n\r\n"), ep="small")))
    p.check("chunk single 1025 over",    is_err(drive_staged(cfg, mock, CHUNKED(b"401\r\n" + b"B" * 0x401 + b"\r\n0\r\n\r\n"), ep="small")))
    p.check("close body at cap 1024",    (lambda r: is_ok(r, 200) and r.get("bodylen") == 1024)(
        drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\n\r\n" + b"B" * 1024, keep=False, ep="small")))
    p.check("header block exactly 256",  is_alive(drive_staged(cfg, mock, hdr_block_bytes(256), ep="small")))

    p.check("body CL at cap (1024)",   (lambda r: is_ok(r, 200) and r.get("bodylen") == 1024)(
        drive_staged(cfg, mock, R("HTTP/1.1 200 OK", ["Content-Length: 1024"], b"B" * 1024), ep="small")))
    p.check("body CL over cap",        is_err(drive_staged(cfg, mock, R("HTTP/1.1 200 OK", ["Content-Length: 1025"], b"B" * 1025), ep="small")))

    p.check("chunk cumulative over cap", is_err(drive_staged(cfg, mock, CHUNKED(b"320\r\n" + b"B" * 0x320 + b"\r\n320\r\n" + b"B" * 0x320 + b"\r\n0\r\n\r\n"), ep="small")))
    p.check("close-delimited over cap",  is_err(drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\n\r\n" + b"B" * 2000, keep=False, ep="small")))
    p.check("trailer count over cap",    is_err(drive_staged(cfg, mock, CHUNKED(b"2\r\nxy\r\n0\r\n" + b"".join(b"X-%d: v\r\n" % i for i in range(20)) + b"\r\n"), ep="small")))

    # Informational cap (fixed at 8 in the client): exact boundary
    p.check("1xx count at cap (8)", is_ok(drive(cfg, "/continue/8"), 200, "done"))
    p.check("1xx count 9 over cap", is_err(drive(cfg, "/continue/9")))
    p.check("1xx count over cap",   is_err(drive(cfg, "/continue/12")))

    # The status line's reason phrase counts toward maxHeaderBytes too: a 300-byte
    # reason on the 256-cap endpoint must be rejected, not silently accepted
    p.check("reason phrase over cap", is_err(drive_staged(cfg, mock,
        ("HTTP/1.1 200 " + "R" * 300 + "\r\nContent-Length: 0\r\n\r\n").encode("latin-1"), ep="small")))

    # Content-Length exactly at cap, then a truncated body + EOF: reserve() runs at
    # the cap, then the parser must error on the short read, never hang, never OOB
    p.check("CL at cap then eof", is_err(drive_staged(cfg, mock,
        b"HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\n" + b"B" * 512, keep=False, ep="small")))
    # A 1xx that hides a body must not desync (client ignores the CL on 1xx)
    p.check("1xx with body rejected", is_err(drive_staged(cfg, mock, b"HTTP/1.1 100 Continue\r\nContent-Length: 3\r\n\r\nXXXHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")), security=True)

# PHASE 9: resource exhaustion, timeouts, coalescing
def _concurrent(fn, n, stagger=0.01):
    # A tiny stagger between thread starts spreads the inbound connection burst so
    # WFX's accept path isn't hit by N simultaneous SYNs (which dropped a few as
    # None). 10ms x N stays well inside the 300ms coalesce window, so coalescing
    # still engages
    out = [None] * n
    def worker(i):
        out[i] = fn(i)
    ts = [threading.Thread(target=worker, args=(i,)) for i in range(n)]
    for t in ts:
        t.start()
        if stagger:
            time.sleep(stagger)
    for t in ts:
        t.join()
    return out

def phase_resource(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("resource")

    # Slow upstreams must surface as request-timeout on the fast (5s-budget) endpoint
    # The outbound stalls 20s, so the timeout (5s + up to a 5s tick) wins; give the
    # inbound read enough slack to receive WFX's eventual timeout response
    t0 = time.time()
    p.check("slow headers -> timeout", is_errc(drive(cfg, "/slow-headers", ep="fast", rtimeout=18), EP_REQ_TIMEOUT), "elapsed %.1fs" % (time.time() - t0))
    p.check("slow body -> timeout",    is_errc(drive(cfg, "/slow-body", ep="fast", rtimeout=18), EP_REQ_TIMEOUT))

    # Pool exhaustion: concurrent slow requests on a 2-slot pool. Every one must
    # resolve (no hang); overflow shows up as pool-exhausted or timeout, never OK
    # Requests queue behind the 2 slots and each burns the full 5s budget (+ up to a
    # 5s tick), so keep the count low and the read window wide enough for two waves
    res = _concurrent(lambda i: drive(cfg, "/slow-headers", ep="fast", rtimeout=30), 4)
    all_resolved = all(is_alive(r) for r in res)
    none_ok = all(not is_ok(r) for r in res)
    p.check("pool exhaustion resolves", all_resolved and none_ok, "results: %r" % res)

    # Coalescing: 16 concurrent identical GETs must hit the backend ONCE
    mock.coalesce_reset()
    res = _concurrent(lambda i: drive(cfg, "/coalesce", ep="coalesce"), 16)
    hits = mock.coalesce_count()
    all_ok = all(is_ok(r, 200, "coalesced") for r in res)
    p.check("coalesce: 16 waiters all ok", all_ok, "results: %r" % res)
    p.check("coalesce: backend hit once", hits == 1, "backend hits = %d (expected 1)" % hits)

    # Control: without coalescing, all 16 reach the backend
    mock.coalesce_reset()
    _concurrent(lambda i: drive(cfg, "/coalesce", ep="default"), 16)
    hits = mock.coalesce_count()
    p.check("no-coalesce: 16 backend hits", hits == 16, "backend hits = %d (expected 16)" % hits)

    # Clone integrity: every coalesced waiter gets its own full 1000-byte copy
    mock.coalesce_reset()
    res = _concurrent(lambda i: drive(cfg, "/coalesce-big", ep="coalesce"), 16)
    good = all(r and r.get("ep") == EP_SUCCESS and r.get("bodylen") == 1000 and r.get("body") == "C" * 1000 for r in res)
    p.check("coalesce clone integrity", good, "a waiter got a truncated/aliased body")

    # Error fan-out: a single failing backend call fails every coalesced waiter
    mock.coalesce_reset()
    res = _concurrent(lambda i: drive(cfg, "/coalesce-bad", ep="coalesce"), 16)
    p.check("coalesce error fan-out", all(is_err(r) for r in res), "results: %r" % res)

    # Two DISTINCT coalesce keys in flight at once: each key must dedupe to its own
    # single backend hit AND every waiter must receive ITS key's body, never the
    # other group's. A key-collision / cross-delivery bug is an info-disclosure leak
    mock.coalesce_reset()
    def fire(i):
        return ("small", drive(cfg, "/coalesce", ep="coalesce")) if i % 2 == 0 \
               else ("big", drive(cfg, "/coalesce-big", ep="coalesce"))
    res = _concurrent(fire, 16)
    hits = mock.coalesce_count()
    small_ok = all(is_ok(r, 200, "coalesced") for tag, r in res if tag == "small")
    big_ok   = all(r and r.get("ep") == EP_SUCCESS and r.get("body") == "C" * 1000 for tag, r in res if tag == "big")
    p.check("coalesce 2 keys no cross-delivery", small_ok and big_ok, "results: %r" % res, security=True)
    p.check("coalesce 2 keys hit twice", hits == 2, "backend hits = %d (expected 2)" % hits)

# PHASE 10: fragmentation (the incremental parser under recv() splitting)
def phase_fragmentation(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("fragmentation")

    def cl(body):
        return ("HTTP/1.1 200 OK\r\nX-Mark: m\r\nContent-Length: %d\r\n\r\n%s"
                % (len(body), body)).encode("latin-1")

    # Whole valid responses delivered one byte at a time: the client must
    # reassemble the status line, each header, the blank line, and the body
    # across a recv() boundary at literally every byte
    p.check("drip CL body",        is_ok(drive_drip(cfg, mock, cl("hello"), piece=1), 200, "hello"))
    p.check("drip empty body",     is_ok(drive_drip(cfg, mock, cl(""), piece=1), 200, ""))
    p.check("drip 2-byte pieces",  is_ok(drive_drip(cfg, mock, cl("abcdefgh"), piece=2), 200, "abcdefgh"))
    p.check("drip chunked",        is_ok(drive_drip(cfg, mock, CHUNKED(b"3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n"), piece=1), 200, "abcde"))
    p.check("drip chunked ext+tr", is_ok(drive_drip(cfg, mock, CHUNKED(b"3;x=y\r\nabc\r\n0\r\nX-T: v\r\n\r\n"), piece=1), 200, "abc"))

    # 1xx interleave delivered fragmented: flags from the 1xx block must not leak
    info = b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone"
    p.check("drip 1xx + final",    is_ok(drive_drip(cfg, mock, info, piece=1), 200, "done"))
    p.check("split 1xx + final",   is_ok(drive_split(cfg, mock, info, 17), 200, "done"))

    # Single split at EVERY byte offset of a fixed CL response: one aggregate check
    blob = cl("SPLITBODY")
    bad_off = None
    for off in range(1, len(blob)):
        if not is_ok(drive_split(cfg, mock, blob, off), 200, "SPLITBODY"):
            bad_off = off
            break
    p.check("split CL at every offset", bad_off is None, "misframed when split at byte %r" % bad_off)

    # Split a chunked response at points inside the size line and inside the data
    ch = CHUNKED(b"5\r\nabcde\r\n3\r\nfgh\r\n0\r\n\r\n")   # body == "abcdefgh"
    base = len(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")
    for k in (0, 1, 3, 5, 8, 12):
        p.check("split chunked @ +%d" % k, is_ok(drive_split(cfg, mock, ch, base + k), 200, "abcdefgh"))

    # Larger body, coarse and fine fragmentation, exact length must survive
    big = cl("Q" * 500)
    p.check("split big body mid",  (lambda r: is_ok(r, 200) and r.get("bodylen") == 500)(drive_split(cfg, mock, big, len(big) // 2)))
    p.check("drip big 8-byte",     (lambda r: is_ok(r, 200) and r.get("bodylen") == 500)(drive_drip(cfg, mock, big, piece=8)))

# PHASE 11: methods (each verb serializes correctly; typed API body rules)
def phase_methods(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("methods")

    for m in ("GET", "OPTIONS", "DELETE"):
        h = raw_head(cfg, method=m)
        p.check("%s request line" % m, bool(h) and h.startswith("%s /reflectraw HTTP/1.1|" % m), "head=%r" % h)
        p.check("%s no CL (no body)" % m, bool(h) and _count_ci(h, "content-length") == 0, "head=%r" % h)

    for m in ("POST", "PUT", "PATCH"):
        h = raw_head(cfg, method=m, x_body="abcd")
        ok = bool(h) and h.startswith("%s /reflectraw HTTP/1.1|" % m) and "|Content-Length: 4|" in h
        p.check("%s body + CL" % m, ok, "head=%r" % h)

    # Every verb round-trips against a live upstream and reflects its own method
    for m in ("GET", "OPTIONS", "DELETE", "POST", "PUT", "PATCH"):
        r = drive(cfg, "/ok", method=m)
        p.check("%s round-trips" % m, is_ok(r, 200, "hello"), "got %r" % r)

    # HEAD stays bodyless even when the upstream advertises a Content-Length
    p.check("HEAD bodyless", is_ok(drive(cfg, "/evil/headbody", method="HEAD"), 200, ""))

# PHASE 12: SECURITY: smuggling, poisoning, cross-request bleed, leaks, DoS
def phase_security(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("security")

    # No cross-request bleed across keep-alive reuse (single-slot pool)
    # Two distinct responses share one connection + parse state. Neither status,
    # body, nor header set may bleed into the other, in either interleaving
    mock.stage("secA", b"HTTP/1.1 201 Created\r\nX-A: aaa\r\nContent-Length: 5\r\n\r\nAAAAA", keep=True)
    mock.stage("secB", b"HTTP/1.1 202 Accepted\r\nX-B: bbb\r\nContent-Length: 3\r\n\r\nBBB", keep=True)
    p.check("reuse A intact",        is_ok(drive(cfg, "/raw/secA", ep="reuse"), 201, "AAAAA"))
    p.check("reuse B intact",        is_ok(drive(cfg, "/raw/secB", ep="reuse"), 202, "BBB"))
    p.check("reuse A->B->A no bleed", is_ok(drive(cfg, "/raw/secA", ep="reuse"), 201, "AAAAA"), security=True)
    p.check("reuse no header bleed",  (lambda r: is_ok(r, 202) and (r.get("hdr") in (None, "")))(
        drive(cfg, "/raw/secB", ep="reuse", want="X-A")), "B surfaced A's header", security=True)

    # Trailer headers must NEVER surface as response headers (trailer smuggle)
    mock.stage("sectr", CHUNKED(b"2\r\nhi\r\n0\r\nX-Trailer-Secret: leak\r\nSet-Cookie: e=1\r\n\r\n"), keep=True)
    p.check("trailer body correct",   is_ok(drive(cfg, "/raw/sectr"), 200, "hi"))
    p.check("trailer secret hidden",  (lambda r: is_ok(r, 200) and (r.get("hdr") in (None, "")))(
        drive(cfg, "/raw/sectr", want="X-Trailer-Secret")), "trailer surfaced as header", security=True)
    p.check("trailer cookie hidden",  (lambda r: is_ok(r, 200) and (r.get("hdr") in (None, "")))(
        drive(cfg, "/raw/sectr", want="Set-Cookie")), "trailer cookie surfaced", security=True)

    # 1xx header block must NOT leak into the final response's headers
    mock.stage("secinfo", b"HTTP/1.1 103 Early Hints\r\nX-Info-Leak: secret\r\nLink: </s>\r\n\r\n"
                          b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", keep=True)
    p.check("1xx final body ok",      is_ok(drive(cfg, "/raw/secinfo"), 200, "hi"))
    p.check("1xx header not leaked",  (lambda r: is_ok(r, 200) and (r.get("hdr") in (None, "")))(
        drive(cfg, "/raw/secinfo", want="X-Info-Leak")), "1xx header leaked into final", security=True)

    # CL-bounded body: extra bytes past Content-Length are NOT part of body
    r = drive_staged(cfg, mock, b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ngoodSMUGGLE!", ep="default", keep=True)
    p.check("CL body exact, extra dropped", is_ok(r, 200, "good") and r.get("bodylen") == 4,
          "delivered past-CL bytes: %r" % r, security=True)

    # Keep-alive poisoning via chunked / no-body-status framings
    poisons = [
        ("chunked trailing smuggle", CHUNKED(b"4\r\ngood\r\n0\r\n\r\n") + b"HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nSMUGGLE"),
        ("204 + TE chunked body",    b"HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"),
        ("304 + TE chunked body",    b"HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"),
        ("204 + hidden CL body",     b"HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\nhelloHTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nBAD"),
    ]
    for name, blob in poisons:
        drive_staged(cfg, mock, blob, ep="small", keep=True)
        p.check("no poison: %s" % name, _reuse_clean(cfg, "small", n=12),
              "pooled connection poisoned by %s" % name, security=True)

    # A 204 whose (illegal) body is delivered LATE, dribbled in after the client has
    # already completed the no-body response and returned the socket to the pool, can't
    # be discarded at completion time, so the phantom bytes land on the idle keep-alive
    # connection. The engine releases a pooled slot that receives unsolicited bytes, but
    # that races with immediate reuse, so the very next request may break. The security
    # guarantees that MUST hold regardless of the race: the phantom bytes are NEVER
    # delivered as a response body (no data smuggle), and the pool recovers (a later
    # request succeeds cleanly). PHANTOM99 is not a valid HTTP response, so if reuse
    # loses the race the poisoned request errors out rather than surfacing the payload
    drive_drip(cfg, mock, b"HTTP/1.1 204 No Content\r\nContent-Length: 9\r\n\r\nPHANTOM99",
               piece=1, ep="small", keep=True)
    outs = [drive(cfg, "/ok", ep="small") for _ in range(10)]
    no_smuggle = all((o is None) or o.get("body") != "PHANTOM99" for o in outs)
    recovered = any(is_ok(o, 200, "hello") for o in outs[-4:])
    p.check("drip 204+body: no data smuggled", no_smuggle,
          "phantom 204 body surfaced as a response", security=True)
    p.check("drip 204+body: pool recovers", recovered,
          "pool never recovered after a late 204 body")

    # Request-side injection breadth: every CR/LF/NUL form must be refused
    path_ctrl = [
        ("bare CR",       b"/a\rb"),      ("bare LF",   b"/a\nb"),   ("CRLF", b"/a\r\nb"),
        ("NUL",           b"/a\x00b"),    ("LF then CR",b"/a\n\rb"), ("CR CR LF", b"/a\r\r\nb"),
        ("smuggle line",  b"/a\r\nHost: evil\r\nGET /x HTTP/1.1\r\n"),
        ("trailing CRLF", b"/a\r\n"),     ("leading CRLF", b"\r\n/a"),
        ("double CRLF",   b"/a\r\n\r\nGET /evil HTTP/1.1"),
    ]
    for name, vec in path_ctrl:
        result = inject(cfg, "path", vec)
        p.check("reject path: %s" % name, is_errc(result, EP_SERIALIZE),
                "expected serialize-err, got %r" % (result,), security=True)

    hdr_ctrl = [
        ("CR value",   b"X-E: a\rb"),   ("LF value", b"X-E: a\nb"),   ("CRLF value", b"X-E: a\r\nb"),
        ("NUL value",  b"X-E: a\x00b"), ("CR name",  b"X-E\rX: b"),   ("LF name",   b"X-E\nX: b"),
        ("NUL name",   b"X-\x00E: b"),
        ("smuggle value", b"X-E: a\r\nContent-Length: 0\r\n\r\nGET /evil HTTP/1.1"),
    ]
    for name, vec in hdr_ctrl:
        result = inject(cfg, "header", vec)
        p.check("reject header: %s" % name, is_errc(result, EP_SERIALIZE),
                "expected serialize-err, got %r" % (result,), security=True)

    # Bytes that are NOT CR/LF/NUL are passed through (documents the exact filter):
    # must neither crash nor smuggle: WFX simply answers
    for name, vec in [("VT", b"/a\x0bb"), ("FF", b"/a\x0cb"), ("DEL", b"/a\x7fb"),
                      ("high 0xFF", b"/a\xffb"), ("tab", b"/a\tb")]:
        p.check("path passthrough: %s" % name, is_alive(inject(cfg, "path", vec)))

    # DoS caps: unbounded input must be refused, not buffered forever
    # A giant line with no newline, dribbled in, must trip maxHeaderBytes
    p.check("no-newline flood capped", is_err(drive_drip(cfg, mock, b"H" * 4096, piece=64, ep="small", keep=False)))
    # Header-count amplification well beyond the cap
    flood = ("HTTP/1.1 200 OK\r\n" + "".join("X-%d: v\r\n" % i for i in range(500)) + "Content-Length: 0\r\n\r\n").encode("latin-1")
    p.check("header-count amplification capped", is_err(drive_staged(cfg, mock, flood, ep="small")))
    # A pile of pipelined responses: only the first is delivered, none smuggled
    burst = b"".join(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nR%02d" % i for i in range(8))
    r = drive_staged(cfg, mock, burst, ep="default", keep=True)
    p.check("pipelined burst first only", is_ok(r, 200) and r.get("body") == "R00",
          "delivered smuggled pipelined response: %r" % r, security=True)

# PHASE 13: connection lifecycle (connect failure/timeout, reconnect, idle, prewarm)
def _wfx_healthy(cfg):
    raw = raw_send(cfg.host, cfg.port, _build("GET", "/health"), rtimeout=2.0, ctimeout=2.0)
    return bool(raw) and _status_of(raw) == 200

def phase_lifecycle(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("lifecycle")

    # Connect refused: nothing listening on the target port. The request must fail
    # cleanly (connect budget + backoff, capped by the 5s request budget + tick),
    # never hang. Bound is generous because timeouts resolve on the 5s timer tick
    t0 = time.time()
    r = drive(cfg, "/ok", ep="dead", rtimeout=18)
    dt = time.time() - t0
    p.check("connect refused -> error", is_err(r) and dt < 16, "elapsed %.1fs r=%r" % (dt, r))

    # Connect timeout: unrouteable TEST-NET address, SYN black-holed. Must surface as
    # an error inside the connect/request budget rather than blocking forever
    t0 = time.time()
    r = drive(cfg, "/ok", ep="unreach", rtimeout=24)
    dt = time.time() - t0
    p.check("connect unreachable -> error", is_err(r) and dt < 22, "elapsed %.1fs r=%r" % (dt, r))

    # The worker must remain healthy after those connect failures
    p.check("worker survives connect fails", _wfx_healthy(cfg) and mock.ping())

    # Reconnect after the upstream drops the connection: request #1 rides a conn the
    # server closes (Connection: close); request #2 must transparently reconnect
    p.check("close then reconnect ok",
          is_ok(drive(cfg, "/close", ep="reuse"), 200, "closebody") and
          is_ok(drive(cfg, "/ok", ep="reuse"), 200, "hello"))

    # Keep-alive reuse: two back-to-back requests ride the SAME pooled connection, so
    # the mock's per-connection request counter climbs 1 -> 2
    r1 = drive(cfg, "/kacount", ep="idle")
    r2 = drive(cfg, "/kacount", ep="idle")
    p.check("keep-alive reuses conn", is_ok(r1, 200, "1") and is_ok(r2, 200, "2"),
          "r1=%r r2=%r" % (r1, r2))

    # Idle timeout: after > idleTimeoutSeconds (5s, fired on the next 5s timer tick,
    # so wait 12s) of inactivity the pooled conn is closed, so the next request opens
    # a fresh one and the per-connection counter resets to 1
    time.sleep(12)
    r3 = drive(cfg, "/kacount", ep="idle")
    p.check("idle timeout recycles conn", is_ok(r3, 200, "1"),
          "expected fresh conn (body '1'), got %r" % r3)

    # Prewarm: EpPrewarm eagerly opened its connections at boot, before any request
    # was driven anywhere. Those connections were accepted by the mock but sent no
    # request, so at boot (snapshot in main) accepted-conns minus served-requests
    # was at least `prewarm` (3)
    idle_prewarmed = getattr(cfg, "prewarm_idle", -1)
    p.check("prewarm opened idle conns", idle_prewarmed >= 3,
          "idle prewarmed conns at boot = %d (expected >= 3)" % idle_prewarmed)

# PHASE 14: onConnect / onDisconnect / multiplexing (raw WFX::Endpoint<>)
#
# HttpEndpoint (every other phase) structurally cannot exercise any of this:
# HTTP/1.1 has no connection handshake and no concept of concurrent requests
# sharing one connection. app/src/main.cpp's ProtoGood/Bad/Slow/Reset instances
# drive the raw primitive against proto_upstream.py's second listener instead
#
# The two security-shaped guarantees this phase actually cares about:
#   - a request must never be served over a connection whose handshake did not
#     complete successfully (auth bypass would be a real vulnerability class);
#   - under multiplexing, one caller must never receive another caller's
#     response (the same "cross-request bleed" class the security phase checks
#     for HTTP, just here at the connection-sharing layer instead)
def phase_protocol(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("protocol")

    proto_disconnects_reset(cfg)

    # onConnect: the handshake gate
    p.check("onConnect success -> value round-trips",
          (lambda r: is_alive(r) and r.get("ep") == EP_SUCCESS and r.get("value") == "alpha")(
              proto_call(cfg, key="alpha", proto="good")))

    p.check("onConnect rejected (bad auth) -> clean failure, never served",
          is_errc(proto_call(cfg, proto="bad"), EP_INTERNAL), security=True)

    p.check("onConnect dropped mid-handshake -> clean failure, never served",
          is_errc(proto_call(cfg, proto="reset"), EP_INTERNAL), security=True)

    t0 = time.time()
    r = proto_call(cfg, proto="slow", rtimeout=20.0)
    dt = time.time() - t0
    p.check("onConnect handshake timeout -> EpHandshakeTimeout",
          is_errc(r, EP_HANDSHAKE_TIMEOUT) and dt < 18, "elapsed %.1fs r=%r" % (dt, r))

    p.check("worker survives every onConnect failure", _wfx_healthy(cfg) and mock.ping())

    # onDisconnect: the right reason for the right scenario
    dc = proto_disconnects(cfg)
    p.check("onDisconnect: handshake-timeout counted", dc.get("handshake", 0) >= 1, "counters=%r" % dc)
    p.check("onDisconnect: error counted (bad + reset)", dc.get("error", 0) >= 2, "counters=%r" % dc)

    # Recovery
    # A rejected, dropped or timed-out handshake must not poison the pool for the NEXT-
    # -legitimate connection attempt
    p.check("good connection still works after prior failures",
          (lambda r: is_alive(r) and r.get("ep") == EP_SUCCESS and r.get("value") == "still-good")(
              proto_call(cfg, key="still-good", proto="good")))

    # Multiplexing
    # N concurrent requests over one connLimit=1 slot, deliberately resolved out of order-
    # -(varied sleep delays): every caller must get back EXACTLY its own value, never-
    # -another caller's, which is the bleed check
    conns_before = mock.proto_conn_count()
    n = 12
    delays = [0.30, 0.02, 0.18, 0.05, 0.25, 0.01, 0.12, 0.28, 0.08, 0.22, 0.03, 0.15]
    joins = [proto_call_async(cfg, key="sleep:%.2f:tok%d" % (delays[i], i), proto="good", rtimeout=10.0)
             for i in range(n)]
    got = [j() for j in joins]

    all_ok = all(is_alive(r) and r.get("ep") == EP_SUCCESS for r in got)
    p.check("multiplexing: all concurrent requests succeed", all_ok, "results=%r" % got)

    matched = all(is_alive(got[i]) and got[i].get("value") == "tok%d" % i for i in range(n))
    p.check("multiplexing: no cross-request bleed (id-matched, not order-matched)", matched,
          "expected tok0..tok%d in order, got %r" % (n - 1, [r.get("value") if r else None for r in got]),
          security=True)

    conns_after = mock.proto_conn_count()
    p.check("multiplexing: shares one connection under load", conns_after - conns_before <= 1,
          "proto connections before=%d after=%d (expected at most +1)" % (conns_before, conns_after))

    # onDisconnect: idle timeout
    # idleTimeoutSeconds=5 (engine floor), fired on the next 5s timer tick, so
    # wait past both. No request rides this connection in the meantime
    time.sleep(11)
    dc2 = proto_disconnects(cfg)
    p.check("onDisconnect: idle timeout counted", dc2.get("idle", 0) >= 1, "counters=%r" % dc2)

# PHASE: slot pinning (Reserve/Release)
#
# Threat model is the connection-pool contamination class: a pooled connection
# handed to a second caller while it still carries the first caller's session
# state (open transaction, SET, LISTEN). Pinning exists to make that impossible,
# so these assert isolation, lifetime, and that coalescing can never merge two
# pinned callers no matter how identical their bytes are
def phase_pinning(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("pinning")

    # Every request on one reservation must land on the SAME physical connection
    # connId comes from the mock's accept counter, so this is observed, not assumed
    r = sp_reserve(cfg, n=4, release="late")
    p.check("reserve: acquired a slot", bool(r) and r.get("reserved") == 1, "r=%r" % r)
    p.check("reserve: all 4 requests on one connection",
          bool(r) and r.get("same") == 1 and r.get("conn", 0) > 0, "r=%r" % r)
    p.check("reserve: every pinned request succeeded",
          bool(r) and r.get("last") == EP_SUCCESS, "r=%r" % r)

    # Two live reservations must be two different connections. If they collapsed
    # onto one, a transaction opened on A would be visible to B
    r = sp_json(cfg, "/sp/reserve/pair", rtimeout=20.0)
    p.check("reserve: two reservations both granted",
          bool(r) and r.get("a_ok") == 1 and r.get("b_ok") == 1, "r=%r" % r)
    p.check("reserve: distinct reservations get distinct connections",
          bool(r) and r.get("distinct") == 1, "r=%r" % r, security=True)
    p.check("reserve: identical bytes on two pins were NOT coalesced",
          bool(r) and r.get("sa") == EP_SUCCESS and r.get("sb") == EP_SUCCESS
          and r.get("conn_a") != r.get("conn_b"), "r=%r" % r, security=True)

    # Release patterns: explicit-early, destructor-late, and double-release must
    # all leave the pool usable. A double release that double-freed the bitmap
    # bit would corrupt the pool for every later caller
    for mode in ("early", "late", "double"):
        r = sp_reserve(cfg, n=2, release=mode)
        p.check("reserve: release=%s completes cleanly" % mode,
              bool(r) and r.get("reserved") == 1 and r.get("same") == 1, "r=%r" % r)

    p.check("reserve: pool still healthy after all release patterns",
          is_ok_sp(sp_get(cfg, key="after-release")), "")

    # Reservations must return to the pool. connLimit is 4, so if any reservation
    # above leaked its slot this loop starves
    ok = True
    for _ in range(8):
        rr = sp_reserve(cfg, n=1, release="early")
        if not rr or rr.get("reserved") != 1:
            ok = False
            break
    p.check("reserve: 8 sequential reservations never exhaust the pool", ok, security=True)

    # Streaming through a pinned connection: the two features must compose, and
    # every chunk must come off the reserved slot rather than a pooled one
    r = sp_json(cfg, "/sp/reserve/stream", {"X-Count": "6"}, rtimeout=30.0)
    p.check("reserve+stream: reservation held for the whole stream",
          bool(r) and r.get("reserved") == 1 and r.get("chunks") == 6, "r=%r" % r)
    p.check("reserve+stream: all chunks from the same pinned connection",
          bool(r) and r.get("same") == 1, "r=%r" % r, security=True)

    p.check("worker healthy after pinning phase", _wfx_healthy(cfg) and mock.ping())

# PHASE: server-initiated push (onPush)
#
# Unsolicited bytes on an idle pooled slot. Before onPush the engine closed the
# connection outright, so this surface is new: a backend that can push must not
# be able to wedge the slot, desync framing, or grow the client without bound
def phase_push(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("push")

    sp_push_reset(cfg)

    # Baseline: pushes land on an idle slot and are decoded there, not mistaken
    # for a response to some later request
    r = sp_get(cfg, key="push:5")
    p.check("push: request itself still answered correctly", is_ok_sp(r), "r=%r" % r)
    time.sleep(0.6)
    st = sp_push_stats(cfg)
    p.check("push: 5 unsolicited messages delivered to onPush",
          st.get("count", 0) >= 5, "stats=%r" % st)
    p.check("push: connection still usable afterwards",
          is_ok_sp(sp_get(cfg, key="after-push")), "")

    # A push arriving WHILE a request is in flight must be consumed by parse(),
    # never routed to onPush. Getting this wrong desyncs framing: the push bytes
    # would be read as part of the response
    sp_push_reset(cfg)
    before = sp_push_stats(cfg).get("count", 0)
    r = sp_get(cfg, key="pushinflight")
    p.check("push: in-flight push handled by parse, request still answered",
          is_alive(r), "r=%r" % r, security=True)
    time.sleep(0.4)
    after = sp_push_stats(cfg).get("count", 0)
    p.check("push: in-flight push did NOT reach onPush (no desync)",
          after == before, "before=%d after=%d" % (before, after), security=True)

    # Partial message with no trailing newline: onPush reports consumed=0 every
    # time. The engine must park and wait for more bytes, not spin on a buffer it
    # cannot drain and not wedge the slot against future use
    sp_push_reset(cfg)
    t0 = time.time()
    r = sp_get(cfg, key="pushpartial")
    p.check("push: partial-message request answered", is_alive(r), "r=%r" % r)
    time.sleep(0.5)
    p.check("push: partial message did not spin or hang the worker",
          _wfx_healthy(cfg) and (time.time() - t0) < 10.0,
          "elapsed %.1fs" % (time.time() - t0), security=True)

    # Undecodable bytes: onPush returns false and the engine closes the slot. The
    # endpoint must recover rather than stay poisoned
    sp_push_reset(cfg)
    r = sp_get(cfg, key="pushgarbage")
    p.check("push: undecodable push answered current request", is_alive(r), "r=%r" % r)
    time.sleep(0.5)
    st = sp_push_stats(cfg)
    p.check("push: undecodable push rejected by handler",
          st.get("rejects", 0) >= 1, "stats=%r" % st)
    p.check("push: endpoint recovers after a rejected push",
          is_ok_sp(sp_get(cfg, key="after-garbage")), "", security=True)

    # Flood: 2000 pushes back to back. The engine must drain them incrementally
    # rather than buffering the whole burst
    sp_push_reset(cfg)
    rss_before = sp_rss(cfg).get("rss", 0)
    r = sp_get(cfg, key="pushflood:2000")
    p.check("push: flood request answered", is_alive(r), "r=%r" % r)
    time.sleep(1.5)
    st = sp_push_stats(cfg)
    rss_after = sp_rss(cfg).get("rss", 0)
    growth = rss_after - rss_before
    p.check("push: flood decoded (>=1500 of 2000)",
          st.get("count", 0) >= 1500, "stats=%r" % st)
    # ~140KB of wire data; multiple MB of RSS growth means it accumulated instead
    p.check("push: flood did not balloon worker memory",
          rss_before == 0 or growth < 8 * 1024 * 1024,
          "rss %d -> %d (+%d)" % (rss_before, rss_after, growth), security=True)

    p.check("worker healthy after push phase", _wfx_healthy(cfg) and mock.ping())

# PHASE: streaming (Stream / CHUNK_READY / CHUNK_READY_FETCH)
#
# The headline property is bounded memory: peak RSS must track ONE chunk, not the
# total response size. The rest guard paths that could quietly break that
# guarantee (abandonment, cursor continuation, concurrency)
def phase_streaming(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("streaming")

    # Server-driven (CHUNK_READY): the mock keeps sending until END
    r = sp_stream(cfg, count=10, size=64, mode="server")
    p.check("stream: server-driven delivered all 10 chunks",
          bool(r) and r.get("chunks") == 10 and r.get("done") == 1, "r=%r" % r)
    p.check("stream: byte count matches 10 x 64",
          bool(r) and r.get("bytes") == 640, "r=%r" % r)

    # Cursor-driven (CHUNK_READY_FETCH): the mock sends NOTHING until asked, so
    # this only completes if the engine re-serialized a continuation each time
    # Same shape as Postgres Execute, Cassandra paging_state, Redis SCAN
    r = sp_stream(cfg, count=10, size=64, mode="fetch")
    p.check("stream: cursor-driven delivered all 10 chunks",
          bool(r) and r.get("chunks") == 10 and r.get("done") == 1, "r=%r" % r)
    p.check("stream: cursor continuation moved the same byte count",
          bool(r) and r.get("bytes") == 640, "r=%r" % r)

    # Both families over identical data must fold to the same checksum, proving
    # chunk boundaries neither drop nor duplicate bytes
    a = sp_stream(cfg, count=8, size=100, mode="server")
    c = sp_stream(cfg, count=8, size=100, mode="fetch")
    p.check("stream: server-driven and cursor-driven agree byte-for-byte",
          bool(a) and bool(c) and a.get("checksum") == c.get("checksum")
          and a.get("checksum", 0) != 0, "server=%r fetch=%r" % (a, c))

    # THE memory assertion: same chunk size, 200x the chunk count. If the engine
    # accumulated chunks rather than reusing one output object, peak RSS scales
    # with total bytes instead of staying flat
    small = sp_stream(cfg, count=50, size=1024, mode="server", rtimeout=60.0)
    rss_small = sp_rss(cfg).get("rss", 0)
    big = sp_stream(cfg, count=10000, size=1024, mode="server", rtimeout=120.0)
    rss_big = sp_rss(cfg).get("rss", 0)
    growth = rss_big - rss_small

    p.check("stream: 50-chunk baseline complete",
          bool(small) and small.get("chunks") == 50, "r=%r" % small)
    p.check("stream: 10000-chunk response complete",
          bool(big) and big.get("chunks") == 10000 and big.get("done") == 1, "r=%r" % big)
    p.check("stream: 10000 chunks moved ~10MB of payload",
          bool(big) and big.get("bytes", 0) >= 10000 * 1024,
          "bytes=%r" % (big or {}).get("bytes"))
    p.check("stream: peak memory bounded by chunk size, not total size",
          rss_small == 0 or growth < 6 * 1024 * 1024,
          "rss %d -> %d (+%d) across 200x more data" % (rss_small, rss_big, growth),
          security=True)

    # Abandonment: caller stops reading after 3 of 500 chunks. The slot must be
    # reclaimed, not stranded holding a half-drained response
    r = sp_stream(cfg, count=500, size=64, mode="server", stop=3, rtimeout=60.0)
    p.check("stream: abandoned after 3 chunks", bool(r) and r.get("chunks") == 3, "r=%r" % r)
    p.check("stream: endpoint usable after abandonment",
          is_ok_sp(sp_get(cfg, key="after-abandon")), "", security=True)
    p.check("stream: a fresh stream works after abandonment",
          (lambda x: bool(x) and x.get("chunks") == 5 and x.get("done") == 1)(
              sp_stream(cfg, count=5, size=32, mode="server")), "")

    # Zero-chunk response: END arrives with no CHUNK at all
    r = sp_stream(cfg, count=0, size=64, mode="server")
    p.check("stream: empty stream terminates cleanly",
          bool(r) and r.get("chunks") == 0 and r.get("done") == 1, "r=%r" % r)

    # Concurrent streams must not cross-deliver. Each asks for a distinct chunk
    # count, so a swapped delivery shows up as a wrong count
    joiners = [sp_async(sp_stream, cfg, count=n, size=32, mode="server", rtimeout=60.0)
               for n in (4, 7, 11, 15)]
    got = [j() for j in joiners]
    p.check("stream: 4 concurrent streams all completed",
          all(bool(x) and x.get("done") == 1 for x in got), "got=%r" % got)
    p.check("stream: concurrent streams got their OWN chunk counts (no cross-delivery)",
          [x.get("chunks") for x in got if x] == [4, 7, 11, 15], "got=%r" % got, security=True)

    p.check("worker healthy after streaming phase", _wfx_healthy(cfg) and mock.ping())

# PHASE: in-band TLS upgrade, the half that needs no certificates
#
# UpgradeToTLS is a generic STARTTLS primitive, so it inherits that family's CVE
# history. Two vectors here need no working handshake and so live in this suite:
#
#   - downgrade-when-required: the server refuses to upgrade and the client must
#     fail closed. MySQL's --ssl (CVE-2015-3152 "BACKRONYM") and pgJDBC
#     (CVE-2025-49146) both continued in plaintext instead, which is what made
#     them MITM-able
#   - agree-then-garbage: the server says yes, then sends bytes that are not a
#     ServerHello. The upgrade must fail and tear the slot down, not hang
#
# The third vector, plaintext buffered across the upgrade boundary
# (CVE-2011-0411 / CVE-2026-41319), needs a real handshake to establish the
# trust boundary it crosses, so it lives in tests/tls_audit
def phase_upgrade(ctx):
    cfg, mock = ctx.cfg, ctx.mock
    p = ctx.phase("upgrade")

    # Server answers "N" to an endpoint whose protocol requires TLS
    t0 = time.time()
    r = sp_get(cfg, key="x", sp="downgrade", rtimeout=20.0)
    dt = time.time() - t0
    p.check("upgrade: refused downgrade fails closed, never plaintext",
          is_alive(r) and r.get("ep") != EP_SUCCESS, "r=%r" % r, security=True)
    p.check("upgrade: downgrade refusal is prompt, not a hang",
          dt < 18.0, "elapsed %.1fs" % dt)

    # Server answers "S" then sends junk instead of a ServerHello
    t0 = time.time()
    r = sp_get(cfg, key="x", sp="tlsgarbage", rtimeout=20.0)
    dt = time.time() - t0
    p.check("upgrade: garbage handshake fails cleanly",
          is_alive(r) and r.get("ep") != EP_SUCCESS, "r=%r" % r, security=True)
    p.check("upgrade: garbage handshake did not hang the worker",
          dt < 18.0 and _wfx_healthy(cfg), "elapsed %.1fs" % dt)

    # A failed upgrade must not poison the endpoint for unrelated traffic
    p.check("upgrade: plaintext endpoint unaffected by failed upgrades",
          is_ok_sp(sp_get(cfg, key="after-upgrade-failures")), "", security=True)

    p.check("worker healthy after upgrade phase", _wfx_healthy(cfg) and mock.ping())

class EndpointAudit(common.Suite):
    name = "endpoint_audit"
    description = "WFX HttpEndpoint audit"
    phases = {
        "framing":    phase_framing,
        "statusline": phase_statusline,
        "headers":    phase_headers,
        "chunked":    phase_chunked,
        "eof":        phase_eof,
        "desync":     phase_desync,
        "serialize":  phase_serialize,
        "limits":     phase_limits,
        "resource":   phase_resource,
        "fragmentation": phase_fragmentation,
        "methods":    phase_methods,
        "security":   phase_security,
        "lifecycle":  phase_lifecycle,
        "protocol":   phase_protocol,
        "pinning":    phase_pinning,
        "push":       phase_push,
        "streaming":  phase_streaming,
        "upgrade":    phase_upgrade,
    }

    def add_arguments(self, parser):
        parser.add_argument("--up-port", type=int, default=8091,
                            help="mock upstream port (MUST match UPSTREAM in app/src/main.cpp)")
        parser.add_argument("--proto-port", type=int, default=8092,
                            help="mock proto-upstream port (MUST match PROTO_UPSTREAM in app/src/proto.cpp)")
        parser.add_argument("--sp-port", type=int, default=8093,
                            help="mock sp-upstream port (MUST match SP_UPSTREAM in app/src/main.cpp)")

    def configure(self, cfg):
        cfg.up_port = cfg.args.up_port
        cfg.proto_port = cfg.args.proto_port
        cfg.sp_port = cfg.args.sp_port

        # These are compiled into the app, so a mismatch means the audit silently tests nothing
        for flag, port, default, source in (("--up-port", cfg.up_port, 8091, "UPSTREAM in app/src/main.cpp"),
                                            ("--proto-port", cfg.proto_port, 8092, "PROTO_UPSTREAM in app/src/proto.cpp"),
                                            ("--sp-port", cfg.sp_port, 8093, "SP_UPSTREAM in app/src/main.cpp")):
            if port != default:
                term.log("runner", _yellow("NOTE: %s=%d must match %s (default %d), the port is baked in "
                                        "at compile time" % (flag, port, source, default)))

    def setup(self, ctx):
        ctx.resources["mock"] = Mock(ctx.cfg)
        ctx.mock.start()

    def before_phases(self, ctx):
        cfg, mock = ctx.cfg, ctx.mock

        # EpPrewarm opens `prewarm` connections eagerly at boot. The mock accepts them but they
        # send nothing, so (accepted conns - served requests) counts idle prewarmed ones. Snapshot
        # it before any request is driven, or later traffic makes it unreadable
        cfg.prewarm_idle = -1
        for _ in range(50):
            cfg.prewarm_idle = mock.idle_conns()
            if cfg.prewarm_idle >= 3:
                break
            time.sleep(0.1)

        term.log("runner", "idle prewarmed connections at boot: %d" % cfg.prewarm_idle)

        # Without this every phase fails for the same uninformative reason
        if not is_ok(drive(cfg, "/ok"), 200, "hello"):
            ctx.phase("preflight").failed("WFX can reach the mock upstream",
                                          "is UPSTREAM in main.cpp == %d?" % cfg.up_port)
            return False

    def teardown(self, ctx):
        if "mock" in ctx.resources:
            ctx.mock.stop()

if __name__ == "__main__":
    common.run(EndpointAudit)
