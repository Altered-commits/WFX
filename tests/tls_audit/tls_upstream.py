#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Hostile TLS mock upstream for the WFX tls_audit
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
# so the parser's guarantees are re-checked end-to-end over TLS
#
# /ctl/stats/<name> also reports session_reused (per-connection, via Python ssl's
# SSLSocket.session_reused) so tls_audit.py can verify WFX's outbound client actually
# resumes a session on reconnect, not just that it completes handshakes

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
    if path.startswith("/ctl/upgrade-inject/"):
        set_upgrade_inject(path.rsplit("/", 1)[1] == "1")
        return _cl("ok")
    if path == "/ctl/stage":
        with _lock:
            _staged[headers.get("x-id", "0")] = (body, headers.get("x-keep", "0") == "1")
        return _cl("staged")

    if path == "/ok":
        return _cl("hello", extra=["X-Mark: alpha"])
    if path == "/empty":
        return _cl("")
    if path.startswith("/cl/"):
        try:
            n = int(path[4:])
        except ValueError:
            n = 0
        return _cl(b"B" * n)
    if path == "/chunked":
        return _raw(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", True)
    if path == "/reflect":
        b = "host=%s|xtest=%s" % (headers.get("host", "-"), headers.get("x-test", "-"))
        return _cl(b)

    # Behavioural attacks
    # Truncation: send a body then hard-RST with no TLS close_notify. A secure client
    # must never treat this as a clean, complete response
    if path == "/truncate":
        return [("send", b"HTTP/1.1 200 OK\r\n\r\npartial-body-then-brutal-reset"), ("reset",)], False
    if path == "/slow":
        return [("send", b"HTTP/1.1 200 OK\r\n"), ("sleep", 30.0)], False

    return _cl("no such route", status="HTTP/1.1 404 Not Found")

def _int(s, d):
    try:
        return int(s)
    except (ValueError, TypeError):
        return d

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

def _hard_reset(sock):
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        sock.close()
    except OSError:
        pass

def serve_conn(raw_sock, ctx, name):
    # TLS handshake first: the moment of truth for the cert/protocol attacks. A WFX
    # client that correctly refuses a hostile cert fails HERE (we count it), so the
    # audit can assert refusal instead of insecure progress
    try:
        sock = ctx.wrap_socket(raw_sock, server_side=True)
    except (ssl.SSLError, OSError):
        _stat(name, "hs_fail")
        try:
            raw_sock.close()
        except OSError:
            pass
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
                try:
                    d = sock.recv(4096)
                except OSError:
                    break
                if not d:
                    break
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
        try:
            sock.close()
        except OSError:
            pass

def make_ctx(cert, key, maxver):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=cert, keyfile=key)
    if maxver == "1.2":
        try:
            ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        except (ValueError, OSError):
            pass
    try:
        ctx.set_alpn_protocols(["http/1.1"])
    except NotImplementedError:
        pass
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

# STARTTLS-style listener: plaintext first, TLS only if the client asks
#
# This exists for one assertion. After answering "S" it appends attacker-supplied
# plaintext BEFORE handing the socket to the TLS layer, exactly the shape of
# CVE-2011-0411 (Postfix) and CVE-2026-41319 (MailKit). The injected line claims
# connId 9999; a client that carries pre-upgrade bytes across the boundary will
# read it as the authenticated peer's AUTH reply, a correct one discards it and
# sees the real id sent inside TLS
_upgrade_seq = 0
_upgrade_lock = threading.Lock()

# Injection is a runtime toggle, not a launch flag, because both halves have to be tested: with it
# off the upgrade must work end to end, with it on the injected bytes must never be trusted
# One listener that can do both also keeps the upstream port baked into the app unchanged
_upgrade_inject = False

def set_upgrade_inject(on):
    global _upgrade_inject
    with _upgrade_lock:
        _upgrade_inject = bool(on)

def serve_upgrade_conn(raw_sock, ctx):
    global _upgrade_seq
    with _upgrade_lock:
        _upgrade_seq += 1
        conn_id = _upgrade_seq
        inject = _upgrade_inject

    raw_sock.settimeout(20.0)
    try:
        buf = b""
        while b"\n" not in buf:
            d = raw_sock.recv(4096)
            if not d:
                return
            buf += d

        if not buf.startswith(b"STARTTLS"):
            return

        raw_sock.sendall(b"S\n")

        if inject:
            # The attack: plaintext appended after the go-ahead, before the
            # handshake. Must never be visible to the protocol post-upgrade
            raw_sock.sendall(b"OK 9999\n")

        try:
            sock = ctx.wrap_socket(raw_sock, server_side=True)
        except (ssl.SSLError, OSError):
            return

        sock.settimeout(20.0)
        buf = b""
        while True:
            while b"\n" not in buf:
                d = sock.recv(4096)
                if not d:
                    return
                buf += d

            line, buf = buf.split(b"\n", 1)
            line = line.decode("latin-1", "replace").rstrip("\r")

            if line.startswith("AUTH"):
                sock.sendall(("OK %d\n" % conn_id).encode())
            elif line.startswith("GET "):
                sock.sendall(("VAL %d:%s\n" % (conn_id, line[4:])).encode())
            else:
                return
    except (OSError, ssl.SSLError):
        return
    finally:
        try:
            raw_sock.close()
        except OSError:
            pass

def upgrade_listener(host, port, ctx):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(256)
    print("tls mock 'upgrade' on %s:%d (inject via /ctl/upgrade-inject/<0|1>)" % (host, port),
          flush=True)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=serve_upgrade_conn, args=(conn, ctx), daemon=True).start()

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

        # kind=upgrade speaks plaintext until the client asks to upgrade
        if kv.get("kind") == "upgrade":
            set_upgrade_inject(kv.get("inject") == "1")
            t = threading.Thread(target=upgrade_listener,
                                 args=(args.host, int(kv["port"]), ctx), daemon=True)
        else:
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