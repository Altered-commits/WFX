#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Scriptable *hostile* mock upstream for the WFX HttpEndpoint audit
#
# This is NOT a conformant HTTP server. It is a raw-socket byte oracle whose whole
# purpose is to attack the WFX client-side HTTP/1.1 parser/serializer in
# include/wfx/endpoint/http.hpp. It speaks exact, hand-built wire bytes so the
# The audit can drive every branch and every boundary of the client
#
# The 150+ attack vectors live in the HARNESS (endpoint_audit.py), not here; this
# file is deliberately thin. It gets hostile bytes onto the wire two ways:
#
#   1. STAGING (/ctl/stage + /raw/<id>): the audit POSTs an arbitrary raw
#      response blob (id in X-Id, keep-alive in X-Keep); WFX then GETs /raw/<id>
#      and the mock replays those exact bytes. This is how the status-line, header,
#      chunk, EOF, and limit fuzz corpora reach the client: one primitive, endless
#      vectors, byte-for-byte controlled by the audit
#
#   2. Fixed routes (/ok, /chunked/<k>, /evil/*, /coalesce, /reflect, ...) for the
#      cases that need stable behaviour or server-side bookkeeping (keep-alive
#      request counter, coalesce backend-hit counter, request reflection)
#
# Threat model: the upstream is fully adversarial. The client must never crash,
# never hang past its timeout, never mis-frame one response into the next, and
# never let a hostile upstream poison a pooled keep-alive connection
#
# Stdlib only. One thread per connection. Pinned by default to 127.0.0.1:8091,
# which the WFX app (app/src/main.cpp, UPSTREAM macro) baked in at compile time

import argparse
import socket
import struct
import threading
import time

_lock = threading.Lock()
_coalesce_hits = 0          # backend hits on /coalesce* since last reset
_total_requests = 0         # every request the mock has served
_total_connections = 0      # every TCP connection the mock has accepted (prewarm proof)
_proto_connections = 0      # every TCP connection accepted by the proto listener (multiplexing proof)
_staged = {}                # id -> (raw_bytes, keep_alive, mode, arg)

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
    if path == "/ctl/protoconns":
        with _lock:
            return _cl(str(_proto_connections))

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

def _int(s, default):
    try:
        return int(s)
    except (ValueError, TypeError):
        return default

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

# Second listener: tiny hand-rolled protocol for onConnect / onDisconnect /
# multiplexing coverage, spoken by tests/endpoint_audit/app/src/proto.cpp's
# raw WFX::Endpoint<> instances (HttpEndpoint can't exercise any of this,
# HTTP/1.1 has no handshake and no concurrent-requests-per-connection)
#
# Wire format, newline-delimited ASCII, one connection = one handshake then any
# number of requests:
#
#   Client -> "AUTH <token>\n"        once, right after connecting
#   Server -> "OK\n"                  token == "good" (or "slow", after a stall)
#          -> "ERR\n"                 token == "bad" (connection stays open,
#                                     the client's onConnect decides what to do)
#          -> (closes, no reply)      token == "reset"
#
#   Client -> "REQ <id> <key>\n"      any number of these, id is caller-assigned
#   Server -> "RES <id> <value>\n"    value == key, unless key is
#                                     "sleep:<secs>:<value>", in which case the
#                                     reply is delayed by <secs> and carries
#                                     <value> instead. Replies are sent from
#                                     independent per-request threads, so with
#                                     more than one request in flight they can,
#                                     and are meant to, come back out of order
def _serve_proto_conn(sock):
    global _proto_connections
    with _lock:
        _proto_connections += 1
    sock.settimeout(65.0)
    write_lock = threading.Lock()
    buf = b""
    authed = False

    def send_line(line):
        with write_lock:
            try:
                sock.sendall(line.encode("latin-1"))
            except OSError:
                pass

    def reply_after(rid, value, delay):
        if delay > 0:
            time.sleep(delay)
        send_line("RES %s %s\n" % (rid, value))

    try:
        while True:
            nl = buf.find(b"\n")
            while nl == -1:
                try:
                    d = sock.recv(4096)
                except OSError:
                    return
                if not d:
                    return
                buf += d
                nl = buf.find(b"\n")

            line = buf[:nl].decode("latin-1", "replace").rstrip("\r")
            buf = buf[nl + 1:]

            if not authed:
                parts = line.split(" ", 1)
                token = parts[1] if len(parts) > 1 else ""
                if token == "good":
                    send_line("OK\n")
                    authed = True
                elif token == "slow":
                    # connectTimeoutSeconds=5 + up to one 5s timer tick = up to 10s worst
                    # case before the engine gives up on its own; stall well past that so
                    # the client's timeout always wins the race, never this reply
                    time.sleep(14.0)
                    send_line("OK\n")
                    authed = True
                elif token == "reset":
                    return  # drop the connection mid-handshake, no reply
                else:
                    send_line("ERR\n")  # e.g. "bad", stay open either way
                continue

            parts = line.split(" ", 2)
            if len(parts) < 3 or parts[0] != "REQ":
                continue
            rid, key = parts[1], parts[2]

            delay, value = 0.0, key
            if key.startswith("sleep:"):
                bits = key.split(":", 2)
                if len(bits) == 3:
                    delay = _float(bits[1], 0.0)
                    value = bits[2]

            threading.Thread(target=reply_after, args=(rid, value, delay), daemon=True).start()
    except OSError:
        return
    finally:
        try:
            sock.close()
        except OSError:
            pass

def _float(s, default):
    try:
        return float(s)
    except (ValueError, TypeError):
        return default

def _serve_proto(host, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(256)
    print("mock proto-upstream on %s:%d" % (host, port), flush=True)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=_serve_proto_conn, args=(conn,), daemon=True).start()

# SP listener: the non-multiplexed protocol that exercises slot pinning,
# streaming (both CHUNK_READY families), onPush, and the cert-free half of the
# TLS-upgrade surface. See app/src/main.cpp for the wire format
#
# Every connection gets a unique connId echoed in "OK <id>", which is what lets
# the audit prove which physical connection served which request, and so
# whether pinning actually pinned
_sp_conn_seq = 0
_sp_mode = {"push_flood": 0, "push_garbage": 0, "push_partial": 0,
            "stall_mid_stream": 0, "push_during_request": 0, "overrun": 0}

def _sp_set(name, value):
    with _lock:
        _sp_mode[name] = value

def _serve_sp_conn(sock):
    global _sp_conn_seq
    with _lock:
        _sp_conn_seq += 1
        conn_id = _sp_conn_seq

    sock.settimeout(65.0)
    buf = b""
    authed = False
    upgraded_probe = False
    page_left = 0
    page_size = 0

    def send(data):
        try:
            sock.sendall(data if isinstance(data, bytes) else data.encode("latin-1"))
        except OSError:
            pass

    def payload(n):
        return ("x" * max(1, n))

    try:
        while True:
            nl = buf.find(b"\n")
            while nl == -1:
                try:
                    d = sock.recv(65536)
                except OSError:
                    return
                if not d:
                    return
                buf += d
                nl = buf.find(b"\n")

            line = buf[:nl].decode("latin-1", "replace").rstrip("\r")
            buf = buf[nl + 1:]

            if line.startswith("STARTTLS"):
                bits = line.split(" ", 1)
                who = bits[1] if len(bits) > 1 else ""

                if who == "tlsgarbage":
                    # Agree to upgrade, then send garbage instead of a
                    # ServerHello: the handshake must fail and tear the slot
                    # down cleanly rather than hang
                    send("S\n")
                    send(b"\x16\x03\x01\x00\x10not-a-tls-handshake\xff" * 8)
                    return

                # Refuse. The "downgrade" endpoint requires TLS, so its
                # onConnect must fail closed instead of continuing in plaintext
                # (the CVE-2015-3152 / CVE-2025-49146 failure mode)
                send("N\n")
                continue

            if not authed:
                parts = line.split(" ", 1)
                token = parts[1] if len(parts) > 1 else ""
                if token in ("good", "tlsgarbage"):
                    send("OK %d\n" % conn_id)
                    authed = True
                elif token == "downgrade":
                    # Only reachable if the client wrongly continued after "N"
                    send("OK %d\n" % conn_id)
                    authed = True
                else:
                    send("ERR\n")
                continue

            if line.startswith("GET "):
                key = line[4:]

                # A push emitted BEFORE the reply arrives while the request is
                # still in flight, so parse() must consume it, never onPush
                if key == "pushinflight":
                    send("PUSH midflight\n")
                    send("VAL %d:%s\n" % (conn_id, key))
                    continue

                # Everything below lands on an idle slot (the reply above ended
                # the request), which is the only state onPush ever sees
                send("VAL %d:%s\n" % (conn_id, key))

                if key.startswith("push:"):
                    for i in range(_int(key.split(":", 1)[1], 1)):
                        send("PUSH n%d\n" % i)
                elif key.startswith("pushflood:"):
                    n = _int(key.split(":", 1)[1], 1000)
                    blob = "".join("PUSH %s\n" % ("f" * 64) for _ in range(n))
                    send(blob)
                elif key == "pushgarbage":
                    # onPush returns false -> engine must close the slot
                    send("NOTAPUSH whatever\n")
                elif key == "pushpartial":
                    # No trailing newline: onPush reports consumed=0 forever
                    # The engine must park, not spin and not wedge the slot
                    send("PUSH incomplete-no-newline")
                continue

            if line.startswith("STREAM "):
                bits = line.split(" ")
                n = _int(bits[1] if len(bits) > 1 else "0", 0)
                sz = _int(bits[2] if len(bits) > 2 else "8", 8)
                for i in range(n):
                    if _sp_mode.get("stall_mid_stream") and i == max(1, n // 2):
                        # Go silent mid-stream: the client's request timeout must
                        # fire instead of hanging forever
                        time.sleep(30.0)
                        return
                    send("CHUNK %s\n" % payload(sz))
                send("END\n")
                continue

            if line.startswith("PAGE "):
                bits = line.split(" ")
                page_left = _int(bits[1] if len(bits) > 1 else "0", 0)
                page_size = _int(bits[2] if len(bits) > 2 else "8", 8)
                if page_left <= 0:
                    send("END\n")
                else:
                    send("CHUNK %s\n" % payload(page_size))
                    page_left -= 1
                continue

            # Cursor continuation. Nothing is sent until the client asks, which
            # is what makes this the CHUNK_READY_FETCH path
            if line == "MORE":
                if page_left <= 0:
                    send("END\n")
                else:
                    send("CHUNK %s\n" % payload(page_size))
                    page_left -= 1
                continue
    except OSError:
        return
    finally:
        try:
            sock.close()
        except OSError:
            pass

def _serve_sp(host, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(256)
    print("mock sp-upstream on %s:%d" % (host, port), flush=True)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=_serve_sp_conn, args=(conn,), daemon=True).start()

# Main
def main():
    ap = argparse.ArgumentParser(description="WFX endpoint-audit hostile mock upstream")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8091)
    ap.add_argument("--proto-port", type=int, default=8092)
    ap.add_argument("--sp-port", type=int, default=8093)
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(256)
    print("mock upstream on %s:%d" % (args.host, args.port), flush=True)

    threading.Thread(target=_serve_proto, args=(args.host, args.proto_port), daemon=True).start()
    threading.Thread(target=_serve_sp, args=(args.host, args.sp_port), daemon=True).start()

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