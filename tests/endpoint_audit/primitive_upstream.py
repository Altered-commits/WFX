#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Mock upstream for the WFX Endpoint<> primitive audit
#
# Two hand-rolled protocols, one listener each, plus a tiny HTTP control plane the
# harness reads counters from. Two are needed because half the primitive is
# unreachable through a multiplexed endpoint, see app/src/main.cpp for exactly why:
#
#   mux   multiplexed. onConnect, onDisconnect, and several requests sharing one
#         physical connection with replies deliberately returned out of order
#   solo  never multiplexed. Slot pinning, both chunk families of streaming,
#         onPush, onAbort's side connection, and the two UpgradeToTLS outcomes
#         that need no certificate: a refusal, and a garbage ServerHello
#
# Both wire formats are documented above their listener here and mirrored in the
# app at app/src/main.cpp. This mock is a byte oracle, not a conformant server: the
# vectors live in endpoint_audit.py.
#
# Stdlib only. One thread per connection.

import argparse
import socket
import threading
import time

_lock = threading.Lock()

# Connections the mux listener has accepted. One multiplexed endpoint drives many
# concurrent requests, so this is what proves they shared a connection
_mux_connections = 0

# Every solo connection gets an id, handed to the client once in "OK <connId>". The
# app keeps it in slot state and stamps it on every response it builds, so which
# physical connection served a request is observed rather than assumed
_solo_conn_seq = 0

# onAbort cancels seen on the solo listener's side connections. The id is the primary
# connection's own AUTH-time connId, so the audit can prove a cancel references the
# same physical connection that was aborted
_solo_cancels = 0
_solo_last_cancel_id = 0

def _int(text, default):
    try:
        return int(text)
    except (ValueError, TypeError):
        return default

def _float(text, default):
    try:
        return float(text)
    except (ValueError, TypeError):
        return default

# Control plane
#
# Plain HTTP, so the harness can read it with the same transport it drives WFX
# with. Counters only: nothing here ever speaks to WFX.
#
#   GET /ctl/ping                 -> "pong"
#   GET /ctl/mux/conns            -> connections the mux listener accepted
#   GET /ctl/solo/cancels         -> "<count> <last connId>"
#   GET /ctl/solo/cancels/reset   -> "ok", zeroes both
def _control_body(path):
    global _solo_cancels, _solo_last_cancel_id

    if path == "/ctl/ping":
        return "pong"

    if path == "/ctl/mux/conns":
        with _lock:
            return str(_mux_connections)

    if path == "/ctl/solo/cancels":
        with _lock:
            return "%d %d" % (_solo_cancels, _solo_last_cancel_id)

    if path == "/ctl/solo/cancels/reset":
        with _lock:
            _solo_cancels = 0
            _solo_last_cancel_id = 0
        return "ok"

    return None

def _serve_control_conn(sock):
    sock.settimeout(10.0)
    buf = b""
    try:
        while b"\r\n\r\n" not in buf:
            data = sock.recv(4096)
            if not data:
                return
            buf += data

        path = ""
        parts = buf.split(b"\r\n", 1)[0].split(b" ")
        if len(parts) > 1:
            path = parts[1].decode("latin-1")

        body = _control_body(path)
        status = "200 OK" if body is not None else "404 Not Found"
        payload = (body if body is not None else "no such control route").encode("latin-1")

        sock.sendall(("HTTP/1.1 %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
                      % (status, len(payload))).encode("latin-1") + payload)
    except OSError:
        return
    finally:
        try:
            sock.close()
        except OSError:
            pass

# The mux listener
#
# Newline-delimited ASCII. One connection is one handshake followed by any number of
# requests:
#
#   Client -> "AUTH <token>\n"      once, right after connecting
#   Server -> "OK\n"                token "good", or "slow" after a long stall
#          -> "ERR\n"               token "bad"; the connection stays open, the
#                                   client's own onConnect decides what to do
#          -> (closes, no reply)    token "reset"
#
#   Client -> "REQ <id> <key>\n"    any number of these, id is caller-assigned
#   Server -> "RES <id> <value>\n"  value is the key, unless the key reads
#                                   "sleep:<secs>:<value>", in which case the reply
#                                   is delayed by <secs> and carries <value>
#
# Replies go out from independent per-request threads, so with more than one request
# in flight they can, and are meant to, come back out of order.
def _serve_mux_conn(sock):
    global _mux_connections
    with _lock:
        _mux_connections += 1

    # Outlive the client's own idle timeout, so the client is always the side that
    # closes an idle pooled connection first. The mock closing first would strand a
    # slot the client still believes is alive, which reads as an intermittent
    # failure rather than as the fixture it is. A peer close still returns EOF
    # immediately, so this only affects genuinely idle sockets
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
                    data = sock.recv(4096)
                except OSError:
                    return
                if not data:
                    return
                buf += data
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
                    # connectTimeoutSeconds is 5, and the engine only notices on its
                    # own 5s timer tick, so the worst case is 10s before it gives up.
                    # Stall past that, so the client's timeout always wins this race
                    time.sleep(14.0)
                    send_line("OK\n")
                    authed = True
                elif token == "reset":
                    return  # drop mid-handshake, no reply
                else:
                    send_line("ERR\n")
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

# The solo listener
#
# Newline-delimited ASCII again, with a handshake the mux protocol has no use for:
#
#   Client -> "STARTTLS <token>\n"  <- "S\n" agree, "N\n" refuse
#          -> "AUTH <token>\n"      <- "OK <connId>\n" | "ERR\n"
#          -> "GET <key>\n"         <- "VAL <connId>:<key>\n"
#          -> "STREAM <n> <sz> <stallAfter> <stallMs>\n"
#                                   <- "CHUNK <payload>\n" * n, then "END\n"
#          -> "PAGE <n> <sz>\n"     <- "CHUNK <payload>\n"
#          -> "MORE\n"              <- "CHUNK <payload>\n", or "END\n" once the
#                                      requested count has run out
#          -> "CANCEL <connId>\n"   <- nothing, the connection just closes
#
# The token in STARTTLS/AUTH names which endpoint instance is connecting, which is
# the only way this mock can tell them apart, and so which behaviour to act out.
# STREAM keeps sending until it runs out, which is the CHUNK_READY path. PAGE
# answers exactly one chunk and then waits, so the engine has to re-serialize a
# "MORE" for every further one, which is the CHUNK_READY_FETCH path. Certain GET
# keys additionally provoke unsolicited "PUSH" lines, listed in _solo_serve_get.
#
# CANCEL needs no handshake, exactly like a real Postgres CancelRequest: it rides a
# brand-new connection and is the only thing that connection ever sends.
def _solo_payload(size):
    return "x" * max(1, size)

def _serve_solo_conn(sock):
    global _solo_conn_seq, _solo_cancels, _solo_last_cancel_id
    with _lock:
        _solo_conn_seq += 1
        conn_id = _solo_conn_seq

    sock.settimeout(65.0)  # same reasoning as the mux listener
    buf = b""
    authed = False
    page_left = 0
    page_size = 0

    def send(data):
        try:
            sock.sendall(data if isinstance(data, bytes) else data.encode("latin-1"))
        except OSError:
            pass

    try:
        while True:
            nl = buf.find(b"\n")
            while nl == -1:
                try:
                    data = sock.recv(65536)
                except OSError:
                    return
                if not data:
                    return
                buf += data
                nl = buf.find(b"\n")

            line = buf[:nl].decode("latin-1", "replace").rstrip("\r")
            buf = buf[nl + 1:]

            if line.startswith("CANCEL "):
                with _lock:
                    _solo_cancels += 1
                    _solo_last_cancel_id = _int(line[7:], 0)
                return

            if line.startswith("STARTTLS"):
                bits = line.split(" ", 1)
                token = bits[1] if len(bits) > 1 else ""

                if token == "tlsgarbage":
                    # Agree to upgrade, then send garbage instead of a ServerHello:
                    # the handshake must fail and tear the slot down cleanly
                    send("S\n")
                    send(b"\x16\x03\x01\x00\x10not-a-tls-handshake\xff" * 8)
                    return

                # Refuse. The "downgrade" instance requires TLS, so its onConnect has
                # to fail closed instead of continuing in plaintext
                send("N\n")
                continue

            if not authed:
                parts = line.split(" ", 1)
                token = parts[1] if len(parts) > 1 else ""

                if token == "abortmidconnect":
                    # A slow AUTH reply gives the audit a window to abandon the client
                    # while onConnect is still awaiting this very reply, which is where
                    # onAbort must not fire: it would steal onConnect's asyncData
                    time.sleep(1.0)
                    send("OK %d\n" % conn_id)
                    authed = True
                elif token == "downgrade":
                    # Only reachable if the client wrongly continued after "N"
                    send("OK %d\n" % conn_id)
                    authed = True
                elif token in ("good", "tlsgarbage", "abort", "abortnoaux"):
                    send("OK %d\n" % conn_id)
                    authed = True
                else:
                    send("ERR\n")
                continue

            if line.startswith("GET "):
                _solo_serve_get(send, conn_id, line[4:])
                continue

            if line.startswith("STREAM "):
                bits = line.split(" ")
                count = _int(bits[1] if len(bits) > 1 else "", 0)
                size = _int(bits[2] if len(bits) > 2 else "", 8)
                stall_after = _int(bits[3] if len(bits) > 3 else "", 0)
                stall_secs = _int(bits[4] if len(bits) > 4 else "", 0) / 1000.0

                for i in range(count):
                    # The stall lets the audit abandon the client after isStreaming is
                    # already set, which is the case onAbort's scope cut covers
                    if stall_after and i == stall_after:
                        time.sleep(stall_secs)
                    send("CHUNK %s\n" % _solo_payload(size))
                send("END\n")
                continue

            if line.startswith("PAGE "):
                bits = line.split(" ")
                page_left = _int(bits[1] if len(bits) > 1 else "", 0)
                page_size = _int(bits[2] if len(bits) > 2 else "", 8)
                if page_left <= 0:
                    send("END\n")
                else:
                    send("CHUNK %s\n" % _solo_payload(page_size))
                    page_left -= 1
                continue

            # Cursor continuation. Nothing goes out until the client asks, which is
            # what makes this the CHUNK_READY_FETCH path
            if line == "MORE":
                if page_left <= 0:
                    send("END\n")
                else:
                    send("CHUNK %s\n" % _solo_payload(page_size))
                    page_left -= 1
                continue
    except OSError:
        return
    finally:
        try:
            sock.close()
        except OSError:
            pass

def _solo_serve_get(send, conn_id, key):
    """Answers one GET. The key doubles as the script for what else the mock does.

    "slow:<secs>"     stall that long before replying
    "push:<n>"        reply, then <n> well-formed pushes onto the now-idle slot
    "pushflood:<n>"   reply, then <n> pushes back to back in one write
    "pushinflight"    a push BEFORE the reply, so parse() has to consume it
    "pushgarbage"     reply, then an undecodable line, so onPush returns false
    "pushpartial"     reply, then a push with no trailing newline
    """
    # A push emitted before the reply arrives is still in flight as far as the slot
    # is concerned, so parse() must consume it and onPush must never see it
    if key == "pushinflight":
        send("PUSH midflight\n")
        send("VAL %d:%s\n" % (conn_id, key))
        return

    # A slow backend. The audit abandons the client well before this fires, giving
    # onAbort a window to run while the primary slot is genuinely mid-request; the
    # reply below still arrives on schedule, exercising "let it finish naturally"
    if key.startswith("slow:"):
        time.sleep(_float(key.split(":", 1)[1], 1.0))

    # Everything below lands on an idle slot, since the reply above ended the
    # request, and an idle slot is the only state onPush ever sees
    send("VAL %d:%s\n" % (conn_id, key))

    if key.startswith("push:"):
        for i in range(_int(key.split(":", 1)[1], 1)):
            send("PUSH n%d\n" % i)
    elif key.startswith("pushflood:"):
        count = _int(key.split(":", 1)[1], 1000)
        send("".join("PUSH %s\n" % ("f" * 64) for _ in range(count)))
    elif key == "pushgarbage":
        send("NOTAPUSH whatever\n")
    elif key == "pushpartial":
        # No trailing newline, so onPush reports consumed=0 every time. The engine
        # must park and wait for more bytes rather than spin on a buffer it cannot
        # drain, and must not wedge the slot against later use
        send("PUSH incomplete-no-newline")

# Listeners
def _listen(host, port, handler, label):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(256)
    print("mock %s listener on %s:%d" % (label, host, port), flush=True)

    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=handler, args=(conn,), daemon=True).start()

def main():
    ap = argparse.ArgumentParser(description="WFX endpoint-audit mock upstream")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--control-port", type=int, default=8091)
    ap.add_argument("--mux-port", type=int, default=8092)
    ap.add_argument("--solo-port", type=int, default=8093)
    args = ap.parse_args()

    threading.Thread(target=_listen, args=(args.host, args.mux_port, _serve_mux_conn, "mux"),
                     daemon=True).start()
    threading.Thread(target=_listen, args=(args.host, args.solo_port, _serve_solo_conn, "solo"),
                     daemon=True).start()

    try:
        _listen(args.host, args.control_port, _serve_control_conn, "control")
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
