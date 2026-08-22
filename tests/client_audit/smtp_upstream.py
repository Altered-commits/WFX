#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Hostile SMTP mock upstream for the WFX client audit
#
# One persona per fixed port, each driving WFX::SmtpEndpoint's onConnect handshake
# (EHLO, STARTTLS, re-EHLO, AUTH) and its transaction phase (MAIL FROM, RCPT TO,
# DATA, body) down one specific hostile path. A client that ever authenticates over
# plaintext, trusts pre-TLS bytes after the upgrade, or hangs on a hostile response
# has a real bug. See client_audit.py for what each persona proves.
#
# Control plane: a small line-based protocol on a dedicated port, not HTTP, since
# this mock speaks raw SMTP on its real ports. "STATS <name>\n" answers with one
# JSON line.

import argparse
import base64
import json
import socket
import ssl
import threading
import time

_lock = threading.Lock()
_stats = {}  # name -> {"handshakes", "hs_fail", "auth_ok", "auth_fail", "bodies": [...]}

def _stat_init(name):
    return _stats.setdefault(name, {"handshakes": 0, "hs_fail": 0, "auth_ok": 0, "auth_fail": 0, "bodies": []})

def _bump(name, key, n=1):
    with _lock:
        _stat_init(name)[key] += n

def _record_body(name, body):
    with _lock:
        _stat_init(name)["bodies"].append(body.decode("latin-1", "replace"))

# Per-connection SMTP state machine

class ConnClosed(Exception):
    pass

class LineReader:
    """Buffered CRLF-line reader over a socket that may be swapped mid-connection (STARTTLS)."""
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def set_sock(self, sock):
        self.sock = sock

    def read_line(self):
        while b"\n" not in self.buf:
            try:
                d = self.sock.recv(65536)
            except OSError:
                raise ConnClosed()
            if not d:
                raise ConnClosed()
            self.buf += d
        line, self.buf = self.buf.split(b"\n", 1)
        return line.rstrip(b"\r")

    def read_data(self):
        """Reads a DATA body until the canonical CRLF-dot-CRLF terminator, un-stuffing
        leading dots. Returns the raw (still CRLF-terminated per line) message bytes."""
        out = bytearray()
        while True:
            line = self.read_line()
            if line == b".":
                return bytes(out)
            if line.startswith(b"."):
                line = line[1:]
            out += line + b"\r\n"

def _send(sock, data, opts):
    delay = opts.get("slow_trickle")
    if delay:
        for i in range(0, len(data), 1):
            sock.sendall(data[i:i + 1])
            time.sleep(delay)
    else:
        sock.sendall(data)

def _send_multiline(sock, opts, code, lines, wrong_code=None):
    """RFC 5321 4.2.1 multi-line response: '-' on every line but the last, ' ' on the last.
    wrong_code, when set, replaces the code on exactly one continuation line, which is
    the probe for a client that reads a spliced response as one reply."""
    for i, text in enumerate(lines):
        is_last = i == len(lines) - 1
        sep = b" " if is_last else b"-"
        use_code = wrong_code if (wrong_code and i == 1) else code
        _send(sock, use_code + sep + text.encode() + b"\r\n", opts)

def _check_auth(user, password, opts):
    return (not opts.get("auth_fail")) and user == opts.get("valid_user", "audituser") and \
        password == opts.get("valid_pass", "audit-pass-123")

def serve_conn(raw_sock, name, opts):
    reader = LineReader(raw_sock)
    sock = raw_sock
    sock.settimeout(opts.get("timeout", 60.0))

    try:
        # 1. Greeting
        if opts.get("malformed_greeting"):
            _send(sock, b"this is not a valid smtp greeting at all\r\n", opts)
            return
        if opts.get("flood_at") == "greeting":
            while True:
                _send(sock, b"220-flood\r\n", opts)
        if opts.get("huge_line_at") == "greeting":
            while True:
                _send(sock, b"A" * 65536, opts)

        _send(sock, b"220 mock.smtp.test ESMTP ready\r\n", opts)
        if opts.get("drop_after") == "greeting":
            return

        # 2. EHLO (pre-TLS)
        line = reader.read_line()
        if not line.upper().startswith(b"EHLO"):
            return

        caps = ["mock.smtp.test at your service"]
        if opts.get("starttls", True):
            caps.append("STARTTLS")
        _send_multiline(sock, opts, b"250", caps)

        if not opts.get("starttls", True):
            # No STARTTLS is offered, so a correct client must never send AUTH or MAIL
            # here. One that does anyway is refused at every command, so a bug in it cannot
            # accidentally look like success
            while True:
                line = reader.read_line()
                _send(sock, b"530 5.7.0 Must issue a STARTTLS command first\r\n", opts)

        # 3. STARTTLS
        line = reader.read_line()
        if not line.upper().startswith(b"STARTTLS"):
            return

        _send(sock, b"220 2.0.0 Ready to start TLS\r\n", opts)

        if opts.get("inject"):
            # CVE-2011-0411 / CVE-2026-41319 class: plaintext appended right after the
            # go-ahead, before the handshake. Must never be visible to the protocol post-upgrade
            _send(sock, opts["inject"], opts)

        if opts.get("drop_after") == "starttls_pre_handshake":
            return

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=opts["cert"], keyfile=opts["key"])
        try:
            sock = ctx.wrap_socket(raw_sock, server_side=True)
        except (ssl.SSLError, OSError) as e:
            _bump(name, "hs_fail")
            print("smtp mock '%s' TLS handshake failed: %r" % (name, e), flush=True)
            return
        _bump(name, "handshakes")
        sock.settimeout(opts.get("timeout", 60.0))
        reader.set_sock(sock)

        if opts.get("drop_after") == "starttls":
            sock.close()
            return

        # 4. Re-EHLO after the upgrade, whose capability list is the only one a correct
        # client may trust
        line = reader.read_line()
        if not line.upper().startswith(b"EHLO"):
            return

        if opts.get("flood_at") == "ehlo2":
            while True:
                _send(sock, b"250-flood\r\n", opts)
        if opts.get("huge_line_at") == "ehlo2":
            while True:
                _send(sock, b"A" * 65536, opts)

        caps2 = ["mock.smtp.test at your service"]
        mechs = opts.get("auth_mechanisms", "PLAIN LOGIN")
        if mechs:
            caps2.append("AUTH %s" % mechs)

        if opts.get("mismatched_code"):
            _send_multiline(sock, opts, b"250", caps2 + ["padding line"], wrong_code=b"251")
        else:
            _send_multiline(sock, opts, b"250", caps2)

        # 5. AUTH
        line = reader.read_line()
        upper = line.upper()
        ok = False
        if upper.startswith(b"AUTH PLAIN"):
            parts = line.split(b" ", 2)
            try:
                blob = base64.b64decode(parts[2]) if len(parts) > 2 else b""
                _, user, password = blob.split(b"\0")
                ok = _check_auth(user.decode("latin-1"), password.decode("latin-1"), opts)
            except (ValueError, IndexError):
                ok = False
        elif upper.startswith(b"AUTH LOGIN"):
            _send(sock, b"334 " + base64.b64encode(b"Username:") + b"\r\n", opts)
            uline = reader.read_line()
            _send(sock, b"334 " + base64.b64encode(b"Password:") + b"\r\n", opts)
            pline = reader.read_line()
            try:
                user = base64.b64decode(uline).decode("latin-1")
                password = base64.b64decode(pline).decode("latin-1")
                ok = _check_auth(user, password, opts)
            except ValueError:
                ok = False
        else:
            _send(sock, b"502 5.5.1 Command not implemented\r\n", opts)
            return

        if ok:
            _bump(name, "auth_ok")
            _send(sock, b"235 2.7.0 Authentication successful\r\n", opts)
        else:
            _bump(name, "auth_fail")
            _send(sock, b"535 5.7.8 Authentication credentials invalid\r\n", opts)
            return

        if opts.get("drop_after") == "auth":
            sock.close()
            return

        # 6. Transaction loop: MAIL FROM / RCPT TO / DATA / body / RSET / QUIT
        while True:
            line = reader.read_line()
            upper = line.upper()

            silent_after = opts.get("silent_after")
            if silent_after and upper.startswith(silent_after.encode()):
                # Goes quiet forever, leaving the client's own requestTimeoutSeconds to
                # end it
                time.sleep(3600)
                return

            if upper.startswith(b"MAIL FROM"):
                _send(sock, b"250 2.1.0 OK\r\n", opts)
            elif upper.startswith(b"RCPT TO"):
                _send(sock, b"250 2.1.5 OK\r\n", opts)
            elif upper.startswith(b"DATA"):
                _send(sock, b"354 Start mail input; end with <CRLF>.<CRLF>\r\n", opts)
                if opts.get("drop_after") == "data_prompt":
                    sock.close()
                    return
                body = reader.read_data()
                _record_body(name, body)
                _send(sock, b"250 2.0.0 OK: queued as 1\r\n", opts)
            elif upper.startswith(b"RSET"):
                _send(sock, b"250 2.0.0 OK\r\n", opts)
            elif upper.startswith(b"QUIT"):
                _send(sock, b"221 2.0.0 Bye\r\n", opts)
                return
            else:
                _send(sock, b"502 5.5.1 Command not implemented\r\n", opts)
    except (ConnClosed, OSError, ssl.SSLError):
        return
    finally:
        try:
            sock.close()
        except OSError:
            pass

def listener(host, port, name, opts):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(256)
    print("smtp mock '%s' on %s:%d" % (name, host, port), flush=True)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=serve_conn, args=(conn, name, opts), daemon=True).start()

# Control plane: line-based, not HTTP, since the real ports speak raw SMTP

def handle_control(conn):
    try:
        conn.settimeout(5.0)
        buf = b""
        while b"\n" not in buf:
            d = conn.recv(4096)
            if not d:
                return
            buf += d
        line = buf.split(b"\n", 1)[0].decode("latin-1", "replace").strip()
        parts = line.split(" ", 1)
        cmd = parts[0] if parts else ""

        with _lock:
            if cmd == "STATS" and len(parts) > 1:
                reply = json.dumps(_stat_init(parts[1]))
            elif cmd == "RESET" and len(parts) > 1:
                _stats.pop(parts[1], None)
                reply = '{"ok":true}'
            else:
                reply = '{"error":"unknown command"}'

        conn.sendall((reply + "\n").encode())
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass

def control_listener(host, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(64)
    print("smtp mock control on %s:%d" % (host, port), flush=True)
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        threading.Thread(target=handle_control, args=(conn,), daemon=True).start()

def parse_listen(spec):
    kv = {}
    for part in spec.split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            kv[k.strip()] = v.strip()
    return kv

_BOOL_KEYS = ("starttls", "mismatched_code", "malformed_greeting", "auth_fail")
_FLOAT_KEYS = ("slow_trickle", "timeout")

def coerce_opts(kv):
    opts = dict(kv)
    for k in _BOOL_KEYS:
        if k in opts:
            opts[k] = opts[k] not in ("0", "false", "False", "")
    for k in _FLOAT_KEYS:
        if k in opts:
            opts[k] = float(opts[k])
    if "inject" in opts:
        opts["inject"] = opts["inject"].encode("latin-1")
    return opts

def main():
    ap = argparse.ArgumentParser(description="WFX smtp_audit hostile SMTP mock")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--control-port", type=int, required=True)
    ap.add_argument("--listen", action="append", required=True,
                    help="name=..,port=..,cert=..,key=..[,starttls=0][,inject=..][,...]")
    args = ap.parse_args()

    threads = [threading.Thread(target=control_listener, args=(args.host, args.control_port), daemon=True)]
    threads[0].start()

    for spec in args.listen:
        kv = parse_listen(spec)
        opts = coerce_opts(kv)
        name = kv.get("name", "l%s" % kv["port"])
        t = threading.Thread(target=listener, args=(args.host, int(kv["port"]), name, opts), daemon=True)
        t.start()
        threads.append(t)

    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
