#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Hostile HTTP mock upstream for the WFX client audit
#
# Not a conformant HTTP server: a raw-socket byte oracle whose whole purpose is to
# attack the client-side HTTP/1.1 parser and serializer in
# include/wfx/endpoint/http.hpp. It speaks exact, hand-built wire bytes, so the
# audit can reach every branch and every boundary of the client.
#
# The vectors live in client_audit.py, not here; this file is deliberately thin. It
# gets hostile bytes onto the wire two ways:
#
#   1. Staging (/ctl/stage then /raw/<id>): the audit POSTs an arbitrary raw
#      response blob, WFX then GETs /raw/<id>, and the mock replays those exact
#      bytes. One primitive, endless vectors, byte for byte controlled by the audit.
#      X-Mode drip or split additionally fragments the delivery
#   2. Fixed routes (/ok, /chunked/<k>, /evil/*, /coalesce, /reflect, ...) for the
#      cases that need stable behaviour or server-side bookkeeping: the keep-alive
#      request counter, the coalesce backend-hit counter, request reflection
#
# Threat model: the upstream is fully adversarial. The client must never crash,
# never hang past its timeout, never mis-frame one response into the next, and
# never let a hostile upstream poison a pooled keep-alive connection.
#
# Stdlib only. One thread per connection. Pinned by default to 127.0.0.1:8091,
# which app/src/main.cpp baked into every endpoint instance at compile time.

import argparse
import socket
import struct
import threading
import time

_lock = threading.Lock()
_coalesce_hits = 0          # backend hits on /coalesce* since last reset
_total_requests = 0         # every request the mock has served
_total_connections = 0      # every TCP connection the mock has accepted (prewarm proof)
_staged = {}                # id -> (raw_bytes, keep_alive, mode, arg)

def _int(s, default):
    try:
        return int(s)
    except (ValueError, TypeError):
        return default

def _bump_total():
    global _total_requests
    with _lock:
        _total_requests += 1

# Response builders. Each returns (plan, keep_alive). A plan is a list of
# actions the connection loop runs in order:
#      ("send", bytes)   write raw bytes
#      ("sleep", secs)   stall (drives the client's request timeout)
#      ("shutwr",)       half-close so the client reads EOF
#      ("reset",)        hard RST via SO_LINGER 0, then close
def _b(s):
    return s.encode("latin-1") if isinstance(s, str) else s

def _resp(status_line, headers, body=b"", keep_alive=True):
    body = _b(body)
    head = status_line + "\r\n" + "".join("%s\r\n" % h for h in headers) + "\r\n"
    return [("send", _b(head) + body)], keep_alive

def _cl(body, status="HTTP/1.1 200 OK", extra=None, keep_alive=True):
    body = _b(body)
    return _resp(status, ["Content-Length: %d" % len(body)] + (extra or []), body, keep_alive)

def _chunk(data):
    data = _b(data)
    return ("%x\r\n" % len(data)).encode("latin-1") + data + b"\r\n"

def _raw(blob, keep_alive=True):
    return [("send", _b(blob))], keep_alive

# Fragmented delivery: put the SAME staged bytes on the wire, but split across
# multiple sends with tiny stalls so the client's incremental parser has to
# reassemble lines/bodies across recv() boundaries. This exercises the lineAcc
# line-join path, the body-resume-across-EpParseIncomplete path, and the
# accumulating maxHeaderBytes cap, none of which a single write() reaches
#   mode "whole" : one send (default)
#   mode "drip"  : arg-byte pieces, each followed by a short sleep
#   mode "split" : one split at byte offset `arg` (0 => midpoint), sleep between
def _raw_frag(blob, keep_alive, mode="whole", arg=0):
    blob = _b(blob)
    if mode == "drip":
        piece = max(1, arg)
        plan = []
        for i in range(0, len(blob), piece):
            plan.append(("send", blob[i:i + piece]))
            plan.append(("sleep", 0.004))
        return (plan or [("send", b"")]), keep_alive
    if mode == "split":
        off = arg if 0 < arg < len(blob) else len(blob) // 2
        return [("send", blob[:off]), ("sleep", 0.02), ("send", blob[off:])], keep_alive
    return [("send", blob)], keep_alive

def handle(method, path, headers, body, conn):
    """Return (plan, keep_alive). conn is per-connection mutable state."""
    global _coalesce_hits

    # Staging: replay audit-supplied raw bytes verbatim
    if path.startswith("/raw/"):
        with _lock:
            entry = _staged.get(path[5:])
        if entry is None:
            return _cl("no such staged id", status="HTTP/1.1 404 Not Found", keep_alive=False)
        blob, keep, mode, arg = entry
        return _raw_frag(blob, keep, mode, arg)

    # Desync / smuggling crown jewels (before the HEAD default)
    # Well-formed-looking responses that hide extra body bytes the client must
    # treat as "no body". If the client reads or leaves those bytes on a pooled
    # connection, the NEXT request on it gets a corrupted / smuggled response
    if path == "/evil/204body":
        return _raw(b"HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\nhello", keep_alive=True)
    if path == "/evil/304body":
        return _raw(b"HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\n\r\nhello", keep_alive=True)
    if path == "/evil/headbody" and method == "HEAD":
        return _raw(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello", keep_alive=True)
    if path == "/evil/trailing":
        return _raw(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ngood"
                    b"HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nSMUGGLE", keep_alive=True)
    if path == "/evil/pipeline":
        return _raw(b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nAAA"
                    b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nBBB", keep_alive=True)

    # HEAD default: every server answers HEAD bodyless. Proves the client
    # completes HEAD on the blank line and keeps the conn reusable even though
    # Content-Length is non-zero
    if method == "HEAD":
        return _raw(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", keep_alive=True)

    # Control plane (the audit hits these directly)
    if path == "/ctl/ping":
        return _cl("pong")
    if path == "/ctl/coalesce/reset":
        with _lock:
            _coalesce_hits = 0
        return _cl("ok")
    if path == "/ctl/coalesce/count":
        with _lock:
            return _cl(str(_coalesce_hits))
    if path == "/ctl/total":
        with _lock:
            return _cl(str(_total_requests))
    if path == "/ctl/conns":
        with _lock:
            return _cl(str(_total_connections))

    # Accepted-connections minus served-requests, computed atomically under one lock
    # in a SINGLE request (this request itself counts as +1 conn and +1 req, netting
    # zero). Counts connections that were opened but never sent a request, i.e. idle
    # prewarmed connections. Reading conns and requests as two separate control calls
    # would skew the diff by 1 (the second call bumps requests once more)
    if path == "/ctl/idleconns":
        with _lock:
            return _cl(str(_total_connections - _total_requests))
    if path == "/ctl/stage":
        with _lock:
            _staged[headers.get("x-id", "0")] = (
                body,
                headers.get("x-keep", "0") == "1",
                headers.get("x-mode", "whole"),
                _int(headers.get("x-arg", ""), 0),
            )
        return _cl("staged")

    # Happy-path framing
    if path == "/ok":
        return _cl("hello", extra=["X-Mark: alpha"])
    if path == "/empty":
        return _cl("")
    if path.startswith("/cl/"):
        return _cl(b"B" * _int(path[4:], 0))
    if path.startswith("/chunked/"):
        k = _int(path[9:], 1)
        parts = [_chunk("[c%d]" % i) for i in range(k)] + [_chunk(b"")]
        return _raw(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + b"".join(parts), True)
    if path == "/chunked-ext":
        return _raw(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    b"3;ext=ignored\r\nabc\r\n0\r\n\r\n", True)
    if path == "/chunked-trailer":
        return _raw(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    b"2\r\nxy\r\n0\r\nX-Trailer: yes\r\n\r\n", True)
    if path == "/close":
        return _resp("HTTP/1.1 200 OK", ["Connection: close"], "closebody", keep_alive=False)
    if path == "/http10":
        return _resp("HTTP/1.0 200 OK", [], "ten", keep_alive=False)

    # Status handling
    if path.startswith("/status/"):
        code = _int(path[8:], 200)
        if code in (204, 304):
            return _resp("HTTP/1.1 %d X" % code, [], b"", keep_alive=True)
        return _cl("s", status="HTTP/1.1 %d X" % code)

    # 1xx informational
    if path == "/continue":
        return _raw(b"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nafter", True)
    if path.startswith("/continue/"):
        k = _int(path[10:], 1)
        return _raw(b"HTTP/1.1 100 Continue\r\n\r\n" * k +
                    b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone", True)

    # Keep-alive proof: body = this connection's 1-based request number
    if path == "/kacount":
        return _cl(str(conn["n"]))

    # Serialize() proof: reflect exactly what the client put on the wire
    if path == "/reflect":
        b = "host=%s|clen=%s|te=%s|blen=%d|xtest=%s" % (
            headers.get("host", "-"), headers.get("content-length", "-"),
            headers.get("transfer-encoding", "-"), len(body), headers.get("x-test", "-"))
        return _cl(b)

    # Serialize() byte oracle: hand back the EXACT request head (request line
    # + header block, minus the trailing blank line) the client emitted, with
    # every CR/LF turned into a '|' (and one trailing '|') so it round-trips
    # cleanly through JSON. Lets the audit assert header order, dedup, CL
    # correctness, and the absence of any smuggled line break
    if path == "/reflectraw":
        h = conn.get("head", b"")
        h = h.replace(b"\r\n", b"|").replace(b"\r", b"|").replace(b"\n", b"|")
        return _cl(h + b"|")

    # Coalescing: count backend hits, stall so waiters pile up
    if path == "/coalesce":
        with _lock:
            _coalesce_hits += 1
        time.sleep(0.30)
        return _cl("coalesced")
    if path == "/coalesce-big":
        with _lock:
            _coalesce_hits += 1
        time.sleep(0.30)
        return _cl(b"C" * 1000)
    if path == "/coalesce-bad":
        with _lock:
            _coalesce_hits += 1
        time.sleep(0.30)
        return _resp("HTTP/1.1 200 OK", ["Content-Length: 5", "Content-Length: 6"], "xxxxx", keep_alive=False)

    # Timeouts / connection faults
    # Stall well past the fast endpoint's 5s request budget (+ up to a 5s timer-tick
    # of slack) so the client's request-timeout, not this response, wins
    if path == "/slow-headers":
        return [("send", b"HTTP/1.1 200 OK\r\n"), ("sleep", 20.0)], False
    if path == "/slow-body":
        return [("send", b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial"), ("sleep", 20.0)], False
    if path == "/drop":
        return [("shutwr",)], False
    if path == "/reset":
        return [("reset",)], False

    return _cl("no such route", status="HTTP/1.1 404 Not Found")

# Connection loop
def _read_headers(sock, buf):
    while b"\r\n\r\n" not in buf:
        try:
            d = sock.recv(4096)
        except OSError:
            return None, b""
        if not d:
            return None, b""
        buf += d
    head, _, rest = buf.partition(b"\r\n\r\n")
    return head, rest

def _parse_request(head):
    lines = head.split(b"\r\n")
    parts = lines[0].split(b" ")
    method = parts[0].decode("latin-1") if parts else ""
    path = parts[1].decode("latin-1") if len(parts) > 1 else ""
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            k, _, v = line.partition(b":")
            headers[k.strip().lower().decode("latin-1")] = v.strip().decode("latin-1")
    return method, path, headers

def serve_conn(sock):
    global _total_connections
    with _lock:
        _total_connections += 1

    # Keep idle keep-alive connections open long enough that the CLIENT (60s idle
    # default) is always the one to close first; otherwise the mock closing an idle
    # pooled connection first would strand a slot the client still thinks is alive,
    # causing intermittent None on reuse. A peer close still returns EOF instantly,
    # so this only affects genuinely-idle keep-alive sockets
    sock.settimeout(65.0)
    conn = {"n": 0}
    buf = b""
    try:
        while True:
            head, buf = _read_headers(sock, buf)
            if head is None:
                return
            method, path, headers = _parse_request(head)

            clen = _int(headers.get("content-length", ""), 0)
            while len(buf) < clen:
                try:
                    d = sock.recv(4096)
                except OSError:
                    break
                if not d:
                    break
                buf += d
            body, buf = (buf[:clen], buf[clen:]) if clen > 0 else (b"", buf)

            conn["n"] += 1
            conn["head"] = head
            _bump_total()

            plan, keep_alive = handle(method, path, headers, body, conn)
            for action in plan:
                if action[0] == "send":
                    sock.sendall(action[1])
                elif action[0] == "sleep":
                    time.sleep(action[1])
                elif action[0] == "shutwr":
                    try:
                        sock.shutdown(socket.SHUT_WR)
                    except OSError:
                        pass
                elif action[0] == "reset":
                    _hard_reset(sock)
                    return

            if not keep_alive or headers.get("connection", "").lower() == "close":
                return
    except OSError:
        return
    finally:
        try:
            sock.close()
        except OSError:
            pass

def _hard_reset(sock):
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        sock.close()
    except OSError:
        pass

# Main
def main():
    ap = argparse.ArgumentParser(description="WFX client-audit hostile HTTP mock upstream")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8091)
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(256)
    print("mock upstream on %s:%d" % (args.host, args.port), flush=True)

    try:
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                break
            threading.Thread(target=serve_conn, args=(conn,), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()

if __name__ == "__main__":
    main()