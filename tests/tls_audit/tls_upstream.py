#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Hostile TLS mock upstream for the WFX tls_audit.
#
# Multiple TLS "personalities" on fixed ports, one per attack on the WFX outbound
# client's certificate/protocol verification:
#
#   good        8443  valid mkcert-trusted cert         -> client MUST accept
#   selfsigned  8444  untrusted self-signed cert         -> client MUST reject
#   wronghost   8445  trusted CA, SAN=evil.example       -> client MUST reject (hostname)
#   expired     8446  trusted CA, notAfter in the past   -> client MUST reject (expired)
#   tls12       8447  valid cert, server capped at 1.2   -> client MUST reject (downgrade)
#
# A client that ACCEPTS any bad one is a MitM hole. The 'good' listener also serves
# the HTTP framing/desync/injection corpus and the truncation/slow-response attacks,
# so the parser's guarantees are re-checked end-to-end over TLS.
#
# /ctl/stats/<name> also reports session_reused (per-connection, via Python ssl's
# SSLSocket.session_reused) so tls_audit.py can verify WFX's outbound client actually
# resumes a session on reconnect, not just that it completes handshakes.

import argparse
import socket
import ssl
import struct
import threading
import time

_lock = threading.Lock()
_stats = {}   # name -> {"handshakes":int, "hs_fail":int, "requests":int}
_staged = {}  # id -> (raw_bytes, keep_alive)

def _stat(name, key, n=1):
    with _lock:
        s = _stats.setdefault(name, {"handshakes": 0, "hs_fail": 0, "requests": 0, "resumed": 0})
        s[key] += n

def _b(s):
    return s.encode("latin-1") if isinstance(s, str) else s

def _resp(status_line, headers, body=b"", keep_alive=True):
    body = _b(body)
    head = status_line + "\r\n" + "".join("%s\r\n" % h for h in headers) + "\r\n"
    return [("send", _b(head) + body)], keep_alive

def _cl(body, status="HTTP/1.1 200 OK", extra=None, keep_alive=True):
    body = _b(body)
    return _resp(status, ["Content-Length: %d" % len(body)] + (extra or []), body, keep_alive)

def _raw(blob, keep_alive=True):
    return [("send", _b(blob))], keep_alive

def handle(name, method, path, headers, body, conn):
    _stat(name, "requests")

    if path.startswith("/raw/"):
        with _lock:
            entry = _staged.get(path[5:])
        if entry is None:
            return _cl("no such id", status="HTTP/1.1 404 Not Found", keep_alive=False)
        blob, keep = entry
        return _raw(blob, keep)

    if method == "HEAD":
        return _raw(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", keep_alive=True)

    if path == "/ctl/ping":
        return _cl("pong")
    if path.startswith("/ctl/stats/"):
        nm = path[len("/ctl/stats/"):]
        with _lock:
            s = _stats.get(nm, {"handshakes": 0, "hs_fail": 0, "requests": 0, "resumed": 0})
            return _cl("%d %d %d %d" % (s["handshakes"], s["hs_fail"], s["requests"], s["resumed"]))
    if path == "/ctl/stage":
        with _lock:
            _staged[headers.get("x-id", "0")] = (body, headers.get("x-keep", "0") == "1")
        return _cl("staged")

    if path == "/ok":
        return _cl("hello", extra=["X-Mark: alpha"])
    if path == "/empty":
        return _cl("")
    if path.startswith("/cl/"):
        try: n = int(path[4:])
        except ValueError: n = 0
        return _cl(b"B" * n)
    if path == "/chunked":
        return _raw(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", True)
    if path == "/reflect":
        b = "host=%s|xtest=%s" % (headers.get("host", "-"), headers.get("x-test", "-"))
        return _cl(b)

    # Behavioural attacks
    # Truncation: send a body then hard-RST with no TLS close_notify. A secure client
    # must never treat this as a clean, complete response.
    if path == "/truncate":
        return [("send", b"HTTP/1.1 200 OK\r\n\r\npartial-body-then-brutal-reset"), ("reset",)], False
    if path == "/slow":
        return [("send", b"HTTP/1.1 200 OK\r\n"), ("sleep", 30.0)], False

    return _cl("no such route", status="HTTP/1.1 404 Not Found")

def _int(s, d):
    try: return int(s)
    except (ValueError, TypeError): return d

def _read_headers(sock, buf):
    while b"\r\n\r\n" not in buf:
        try: d = sock.recv(4096)
        except OSError: return None, b""
        if not d: return None, b""
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

def _hard_reset(sock):
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        sock.close()
    except OSError:
        pass

def serve_conn(raw_sock, ctx, name):
    # TLS handshake first: the moment of truth for the cert/protocol attacks. A WFX
    # client that correctly refuses a hostile cert fails HERE (we count it), so the
    # harness can assert refusal instead of insecure progress.
    try:
        sock = ctx.wrap_socket(raw_sock, server_side=True)
    except (ssl.SSLError, OSError):
        _stat(name, "hs_fail")
        try: raw_sock.close()
        except OSError: pass
        return

    _stat(name, "handshakes")
    if getattr(sock, "session_reused", False):
        _stat(name, "resumed")
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
                try: d = sock.recv(4096)
                except OSError: break
                if not d: break
                buf += d
            body, buf = (buf[:clen], buf[clen:]) if clen > 0 else (b"", buf)

            conn["n"] += 1
            plan, keep_alive = handle(name, method, path, headers, body, conn)
            for action in plan:
                if action[0] == "send":
                    sock.sendall(action[1])
                elif action[0] == "sleep":
                    time.sleep(action[1])
                elif action[0] == "reset":
                    _hard_reset(sock)
                    return
            if not keep_alive or headers.get("connection", "").lower() == "close":
                return
    except OSError:
        return
    finally:
        try: sock.close()
        except OSError: pass

def make_ctx(cert, key, maxver):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=cert, keyfile=key)
    if maxver == "1.2":
        try: ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        except (ValueError, OSError): pass
    try: ctx.set_alpn_protocols(["http/1.1"])
    except NotImplementedError: pass
    return ctx

def listener(host, port, ctx, name):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(256)
    print("tls mock '%s' on %s:%d" % (name, host, port), flush=True)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=serve_conn, args=(conn, ctx, name), daemon=True).start()

def parse_listen(spec):
    kv = {}
    for part in spec.split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            kv[k.strip()] = v.strip()
    return kv

# Main
def main():
    ap = argparse.ArgumentParser(description="WFX tls_audit hostile TLS mock")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--listen", action="append", required=True,
                    help="name=..,port=..,cert=..,key=..[,maxver=1.2]")
    args = ap.parse_args()

    threads = []
    for spec in args.listen:
        kv = parse_listen(spec)
        ctx = make_ctx(kv["cert"], kv["key"], kv.get("maxver", ""))
        t = threading.Thread(target=listener,
                             args=(args.host, int(kv["port"]), ctx, kv.get("name", "l%s" % kv["port"])),
                             daemon=True)
        t.start()
        threads.append(t)

    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()