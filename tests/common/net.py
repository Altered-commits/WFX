# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Raw HTTP over plain TCP or TLS
#
# Deliberately hand-rolled rather than http.client or requests: these suites send malformed
# framing, forged headers and truncated bodies on purpose, and any real client would normalise or
# reject exactly the things under test. Nothing here raises, a failed exchange is None

import json
import socket
import ssl
import time

def _drain(sock, rtimeout, rmax):
    """Reads until the peer closes, the read times out, or rmax bytes arrive. Never raises."""
    sock.settimeout(rtimeout)

    chunks, total = [], 0
    while total < rmax:
        try:
            data = sock.recv(65536)
        except (socket.timeout, OSError):
            break
        if not data:
            break
        chunks.append(data)
        total += len(data)

    return b"".join(chunks)

def _close(sock):
    if sock is not None:
        try:
            sock.close()
        except OSError:
            pass

def send(host, port, payload, rtimeout=8.0, ctimeout=5.0, rmax=8 << 20, tls=False, sni="localhost"):
    """Write payload, read until close or timeout. Returns raw bytes, or None if it never landed."""
    sock = None
    try:
        sock = socket.create_connection((host, port), timeout=ctimeout)
        if tls:
            sock = ssl._create_unverified_context().wrap_socket(sock, server_hostname=sni)
    except (OSError, ssl.SSLError):
        _close(sock)
        return None

    try:
        sock.sendall(payload)
        return _drain(sock, rtimeout, rmax)
    except (OSError, ssl.SSLError):
        return None
    finally:
        _close(sock)

def send_dripped(host, port, payload, chunk_size=1, delay=0.0, rtimeout=5.0, ctimeout=4.0,
                 rmax=8 << 20):
    """Writes the payload in small pieces, for slow-send and partial-request vectors.

    A parser that buffers correctly cannot tell this from a single write; one that assumes a whole
    request per read falls apart, which is the point.
    """
    sock = None
    try:
        sock = socket.create_connection((host, port), timeout=ctimeout)
    except OSError:
        return None

    try:
        for i in range(0, len(payload), chunk_size):
            sock.sendall(payload[i:i + chunk_size])
            if delay:
                time.sleep(delay)

        return _drain(sock, rtimeout, rmax)
    except OSError:
        return None
    finally:
        _close(sock)

def send_and_abandon(host, port, payload, ctimeout=5.0, hold=0.0):
    """Connects, writes the payload, then vanishes without ever reading a reply.

    Simulates a client that bails mid-request (browser tab closed, curl ^C'd). `hold`
    sleeps before the socket closes, giving the server time to actually dispatch the
    request to whatever it's proxying before the disconnect lands. Never raises.
    """
    sock = None
    try:
        sock = socket.create_connection((host, port), timeout=ctimeout)
        sock.sendall(payload)
        if hold:
            time.sleep(hold)
    except OSError:
        pass
    finally:
        _close(sock)

def request(method, path, headers=None, body=b""):
    """Builds a well-formed request. Suites testing malformed framing build their own bytes."""
    if isinstance(body, str):
        body = body.encode("latin-1")

    lines = ["%s %s HTTP/1.1" % (method, path), "Host: h", "Connection: close"]
    for key, value in (headers or {}).items():
        lines.append("%s: %s" % (key, value))
    if body or method in ("POST", "PUT", "PATCH"):
        lines.append("Content-Length: %d" % len(body))

    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body

# Response accessors, all None/empty tolerant so callers can chain them on a failed send
def status(raw):
    if not raw or not raw.startswith(b"HTTP/"):
        return None
    try:
        return int(raw.split(b" ", 2)[1])
    except (IndexError, ValueError):
        return None

def body(raw):
    if not raw:
        return b""
    i = raw.find(b"\r\n\r\n")
    return raw[i + 4:] if i >= 0 else b""

def headers(raw):
    """(status_line, {lowercased name: [values]}, body). Values are lists, since duplicate headers
    are exactly what several suites are testing for."""
    i = raw.find(b"\r\n\r\n") if raw else -1
    if i < 0:
        return None, {}, raw or b""

    lines = raw[:i].split(b"\r\n")
    parsed = {}
    for line in lines[1:]:
        if b":" not in line:
            continue
        name, _, value = line.partition(b":")
        parsed.setdefault(name.strip().lower(), []).append(value.strip())

    return (lines[0] if lines else None), parsed, raw[i + 4:]

def dechunk(raw_body):
    """Decodes chunked transfer-encoding. Stops at the terminator or the first malformed size."""
    out = bytearray()
    i = 0

    while i < len(raw_body):
        j = raw_body.find(b"\r\n", i)
        if j < 0:
            break

        try:
            size = int(raw_body[i:j].split(b";")[0].strip(), 16)
        except ValueError:
            break
        if size == 0:
            break

        start = j + 2
        out += raw_body[start:start + size]
        i = start + size + 2

    return bytes(out)

def get_json(host, port, method, path, headers=None, payload=b"", rtimeout=30.0, tls=False):
    """Round-trips a request and decodes a JSON body. None on any failure along the way."""
    raw = send(host, port, request(method, path, headers, payload), rtimeout=rtimeout, tls=tls)
    if not raw or status(raw) != 200:
        return None

    try:
        return json.loads(body(raw))
    except (ValueError, TypeError):
        return None
