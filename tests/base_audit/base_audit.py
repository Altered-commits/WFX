#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX base audit
#
# See README.md for what each phase proves.
#
# Usage:
#   python3 base_audit.py                   # all phases
#   python3 base_audit.py --phase security
#   python3 base_audit.py --list-phases
#
# Exit codes:
#   0   all phases passed
#   1   crash / hang / correctness failure / server death
#   2   security finding (traversal, response splitting, info leak)

import concurrent.futures
import json
import os
import random
import signal
import socket
import sys
import threading
import time

# Suites are run directly, so tests/ has to be on the path before common is importable
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

import common
from common import net, term

_green, _red, _yellow, _cyan = term.green, term.red, term.yellow, term.cyan

# Transport
#
# common.net is the transport; these adapt it to the (status, raw) pair this suite reads on
# nearly every line, and build the request bytes its vectors need
# A failed exchange is ("CONN_ERR", b""): refused and reset used to be distinguished, but every
# call site treats them the same ("not ready yet")
_status    = net.status
body_of    = net.body
parse_hdrs = net.headers

def raw_send(host, port, payload, rtimeout=3.0, rmax=1 << 20, ctimeout=4.0):
    raw = net.send(host, port, payload, rtimeout=rtimeout, ctimeout=ctimeout, rmax=rmax)
    return ("CONN_ERR", b"") if raw is None else (net.status(raw), raw)

# Writes the request in small pieces, so it lands over several server-side reads instead of one:-
# -exercises the ET-epoll multi-read and incremental buffer growth paths
def raw_send_dripped(host, port, payload, chunk_size=1, delay=0.0, rtimeout=5.0, rmax=1 << 20,
                     ctimeout=4.0):
    raw = net.send_dripped(host, port, payload, chunk_size=chunk_size, delay=delay,
                           rtimeout=rtimeout, ctimeout=ctimeout, rmax=rmax)
    return ("CONN_ERR", b"") if raw is None else (net.status(raw), raw)

# Reads the reply in small pieces, so the server's non-blocking socket writes actually hit EAGAIN
# instead of draining into the kernel buffer in one call: exercises Flush()'s backpressure path
def raw_recv_dripped(host, port, payload, chunk_size=8192, delay=0.003, rtimeout=30.0, rmax=64 << 20,
                     ctimeout=5.0):
    raw = net.recv_dripped(host, port, payload, chunk_size=chunk_size, delay=delay,
                           rtimeout=rtimeout, ctimeout=ctimeout, rmax=rmax)
    return ("CONN_ERR", b"") if raw is None else (net.status(raw), raw)

# Counts actual chunk-size header lines in a raw (still chunk-framed) body, stopping at the
# terminator. net.dechunk() only returns the decoded bytes, this proves the framing itself: how
# many real chunks were on the wire, not just whether they decode to the right content
def count_chunks(raw_body):
    n, i = 0, 0
    while i < len(raw_body):
        j = raw_body.find(b"\r\n", i)
        if j < 0:
            break
        size = int(raw_body[i:j].split(b";")[0].strip(), 16)
        if size == 0:
            break
        n += 1
        i = j + 2 + size + 2
    return n

def _build(method, path, headers=None, body=b"", close=True):
    lines = ["%s %s HTTP/1.1" % (method, path), "Host: x"]
    if close:
        lines.append("Connection: close")
    if headers:
        for k, v in headers.items():
            lines.append("%s: %s" % (k, v))
    if body:
        lines.append("Content-Length: %d" % len(body))

    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body

def _build_at_header_size(total, method="GET", path="/health"):
    """Build a request whose header block is exactly `total` bytes, via one padding header.

    The block runs from the request line through the blank line inclusive, matching
    SafeFindHeaderEnd's accounting in http_parser.cpp, so a boundary test can target
    max_header_size precisely instead of guessing padding by hand.
    """
    head = "%s %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n" % (method, path)
    tail = "\r\n"
    pad_prefix, pad_suffix = "X-Pad: ", "\r\n"

    pad_len = total - (len(head) + len(tail) + len(pad_prefix) + len(pad_suffix))
    if pad_len < 0:
        raise ValueError("target header size %d too small" % total)

    return (head + pad_prefix + ("A" * pad_len) + pad_suffix + tail).encode("latin-1")

def req(host, port, method, path, headers=None, body=b"", **kw):
    return raw_send(host, port, _build(method, path, headers, body), **kw)

# Process inspection
_HAS_PROC = os.path.isdir("/proc")

def children(ppid):
    if not _HAS_PROC:
        return []
    out = []
    try:
        for e in os.listdir("/proc"):
            if not e.isdigit():
                continue
            try:
                with open("/proc/%s/status" % e) as f:
                    for line in f:
                        if line.startswith("PPid:"):
                            if int(line.split()[1]) == ppid:
                                out.append(int(e))
                            break
            except (OSError, ValueError):
                pass
    except OSError:
        pass
    return out

def rss_mb(pids):
    if not _HAS_PROC:
        return 0.0
    total = 0
    for p in pids:
        try:
            with open("/proc/%d/status" % p) as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        total += int(line.split()[1])
                        break
        except (OSError, ValueError):
            pass
    return total / 1024.0

def fd_count(pids):
    if not _HAS_PROC:
        return 0
    total = 0
    for p in pids:
        try:
            total += len(os.listdir("/proc/%d/fd" % p))
        except OSError:
            pass
    return total

def proc_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False

# Server context
# Payload corpora
# Leak markers: any of these in a response = information leakage finding
# Pairs: (must_contain_a, must_contain_b_or_None)
LEAK_MARKERS = [
    (b"root:x:",           None),
    (b"root:",             b":/bin"),
    (b"root:",             b":/root"),
    (b"nobody:",           b":/"),
    (b"daemon:",           b":/"),
    (b"www-data:",         b":/"),
    (b"shadow:",           None),
    (b"NOPASSWD",          None),
    (b"PATH=",             b"HOME="),
    (b"PATH=",             b"USER="),
    (b"LD_LIBRARY",        None),
    (b"LD_PRELOAD",        None),
    (b"PRIVATE KEY",       None),
    (b"BEGIN CERTIFICATE", None),
    (b"BEGIN RSA",         None),
    (b"BEGIN EC PRIVATE",  None),
    (b"RegisterMasterAPI", None),
    (b"WFX_GET(\"/violate",None),
    (b"preferred_config",  None),
    (b"max_connections_per_ip", None),
    (b"r-xp",              b"/lib"),
    (b"r-xp",              b"wfx"),
    (b"wfx\x00run",        None),
    (b"wfx\x00control",    None),
    (b"#include <wfx",     None),
    (b"WFX_GET(\"/health", None),
]

def has_leak(raw):
    for a, b in LEAK_MARKERS:
        if a in raw and (b is None or b in raw):
            return True, a
    return False, None

def _build_traversal_url():
    targets = [
        "etc/passwd", "etc/shadow", "etc/crontab", "etc/hosts",
        "etc/sudoers", "etc/group", "etc/os-release",
        "proc/self/environ", "proc/self/cmdline", "proc/self/maps", "proc/self/fd/0",
        "app/config/wfx.local.toml", "app/src/main.cpp",
    ]
    vecs = []
    for t in targets:
        d = t.count("/") + 4
        dd = "../" * d
        vecs += [
            "/public/" + dd + t,
            "/public/" + "../" * 20 + t,
            "/public/test/foo/" + dd + t,
            "/public/./.././.././../" + t,
        ]
    for t in ["etc/passwd", "etc/shadow", "proc/self/environ"]:
        d = t.count("/") + 4
        vecs += [
            "/public/" + ("%2e%2e%2f" * d) + t,
            "/public/" + ("%2e%2e/" * d) + t,
            "/public/" + ("..%2f" * d) + t,
            "/public/" + ("%2e%2e%2f" * d) + t.replace("/", "%2f"),
        ]
    # Double percent-encoding
    vecs += ["/public/" + ("..%252f" * 5) + "etc/passwd",
             "/public/" + ("%252e%252e/" * 5) + "etc/passwd"]
    # Overlong UTF-8 (/ = 0xC0 0xAF, . = 0xC0 0xAE)
    for t in ["etc/passwd", "proc/self/environ"]:
        vecs += [
            "/public/" + ("..%c0%af" * 5) + t,
            "/public/" + ("..%e0%80%af" * 5) + t,
            "/public/" + ("%c0%ae%c0%ae%c0%af" * 5) + t,
        ]
    # Unicode fullwidth / RTL override
    vecs += [
        "/public/..%ef%bc%8f..%ef%bc%8fetc%ef%bc%8fpasswd",
        "/public/%e2%80%ae..%2fetc%2fpasswd",
    ]
    # Backslash
    for t in ["etc\\passwd", "etc/passwd"]:
        vecs += [
            "/public/..%5c..%5c..%5c" + t,
            "/public/..\\..\\..\\" + t,
            "/public/..%2F..\\..%5c..%2F" + t,
        ]
    # Multiple slashes, semicolons, repeated dots, dot tricks, null bytes,
    # query/fragment, tab, root-relative, Windows-style
    vecs += [
        "/public//../../etc/passwd",
        "/public/..//////etc/passwd",
        "//etc/passwd",
        "/public/..///..//etc/passwd",
        "/public/..;/..;/..;/etc/passwd",
        "/public/;/../../../etc/passwd",
        "/public/foo;/../../../etc/passwd",
        "/public/..;/../etc/passwd",
        "/public/....//....//....//....//etc/passwd",
        "/public/..../..../..../etc/passwd",
        "/public/.%2e/.%2e/.%2e/etc/passwd",
        "/public/.%2e%2f.%2e%2f.%2e%2fetc/passwd",
        "/public/./../../etc/passwd",
        "/public/%00../../../../etc/passwd",
        "/public/../../../../etc/passwd%00.txt",
        "/public/foo%00../../etc/passwd",
        "/public/..%00../etc/passwd",
        "/public/../../../../etc/passwd\x00.png",
        "/public/../../../etc/passwd?q=1",
        "/public/../../../etc/passwd#frag",
        "/public/../../../etc/passwd%23inject",
        "/public/../../../etc/passwd%3fq=1",
        "/public/%09../../etc/passwd",
        "/%2e%2e/%2e%2e/%2e%2e/etc/passwd",
        "/..%2f..%2f..%2f..%2fetc/passwd",
        "/../../../etc/passwd",
        "////etc/passwd",
        "/public/C:\\Windows\\system32\\drivers\\etc\\hosts",
        "/public/%43%3a%5cwindows%5csystem32%5cdrivers%5cetc%5chosts",
    ]
    seen = set()
    out = []
    for v in vecs:
        if v not in seen:
            seen.add(v)
            out.append(v)
    return out

def _build_traversal_hdr():
    targets = [
        "etc/passwd", "etc/shadow", "etc/crontab", "etc/hosts",
        "etc/sudoers", "etc/group", "etc/os-release",
        "proc/self/environ", "proc/self/cmdline", "proc/self/maps", "proc/self/fd/0",
        "app/config/wfx.local.toml", "app/src/main.cpp",
    ]
    vecs = []
    for t in targets:
        d = t.count("/") + 4
        vecs += ["../" * d + t, "../" * 15 + t]
    for p in ["/etc/passwd", "/etc/shadow", "/etc/hosts", "/etc/sudoers",
              "/proc/self/environ", "/proc/self/cmdline", "/proc/self/maps", "/proc/self/fd/0"]:
        vecs.append(p)
    for t in ["etc/passwd", "proc/self/environ"]:
        vecs += ["..%2f" * 5 + t, "%2e%2e/" * 5 + t, "..%252f" * 5 + t]
    vecs += [
        "..%c0%af..%c0%afetc/passwd",
        "..%ef%bc%8f..%ef%bc%8fetc%ef%bc%8fpasswd",
        "....//....//....//etc/passwd",
        "..../..../..../etc/passwd",
        "..\\..\\..\\..\\etc\\passwd",
        "..\\..\\/etc/passwd",
        "..%5c..%5c..%5cetc%5cpasswd",
        "../../../../../../etc/passwd%00.txt",
        "..%00/../../../etc/passwd",
        "../../etc/passwd\x00.png",
        "../../etc/passwd\x00.jpg",
        ";../../../etc/passwd",
        "../;/../../../etc/passwd",
        "..;/..;/..;/etc/passwd",
        "../app/../../../etc/passwd",
        "../app/../../../../../../etc/shadow",
        "public/../../../etc/passwd",
        "../" * 64 + "etc/passwd",
    ]
    seen = set()
    out = []
    for v in vecs:
        if v not in seen:
            seen.add(v)
            out.append(v)
    return out

TRAVERSAL_URL = _build_traversal_url()
TRAVERSAL_HDR = _build_traversal_hdr()

# CRLF corpus: check is STRICT, only flags if the marker appears as a parsed
# HEADER NAME (split on \r\n, partitioned at first :). Substrings inside
# values never count. Zero false positives
CRLF_VALUES = [
    # Real CRLF injection: the only ones that actually split in HTTP/1.1
    b"safe\r\nX-Injected: pwned",
    b"safe\r\n\r\nX-Injected: pwned",
    b"safe\r\nX-Injected: pwned\r\nX-Extra: yes",
    b"safe\r\nContent-Type: text/html",
    b"safe\r\nSet-Cookie: session=evil; Path=/; HttpOnly",
    b"safe\r\nLocation: http://evil.example/",
    b"safe\r\nTransfer-Encoding: chunked",
    b"safe\r\nContent-Length: 0",
    # Bare LF: some parsers accept as line terminator
    b"safe\nX-Injected: pwned",
    # Null + CRLF: null-terminated string bypass
    b"safe\x00\r\nX-Injected: pwned",
    # Double CRLF: response body injection
    b"safe\r\n\r\nHTTP/1.1 200 Injected\r\nX-Injected: pwned\r\n\r\nbody",
    # Non-splitters (verify no false positive):
    b"safe\rX-Injected: pwned",          # bare CR is NOT a header terminator
    b"safe\tX-Injected: pwned",          # tab is NOT a line terminator
    b"\x00X-Injected: pwned",            # null alone is NOT
    b"safe\xe2\x80\xa8X-Injected: pwned",  # U+2028 NOT
    b"safe\xe2\x80\xa9X-Injected: pwned",  # U+2029 NOT
    # Long values: no injection, just stress the header buffer
    b"A" * 8192,
    b"A" * 16384,
    b"A" * 32768,
]

# Max body size from wfx.toml = 65536
# Test at-limit, one-over, and integer-overflow variants
_BODY_LIMIT = 65536
# Max header block size from wfx.toml (max_header_size) = 8192
_HEADER_LIMIT = 8192
BODYBOMB_PAYLOADS = [
    # Exactly at limit: should be accepted (200)
    _build("POST", "/echo-body", body=b"B" * _BODY_LIMIT),
    # One byte over: must be rejected cleanly (400), not crash
    _build("POST", "/echo-body", body=b"B" * (_BODY_LIMIT + 1)),
    # Moderately over
    _build("POST", "/echo-body", body=b"B" * (_BODY_LIMIT * 2)),
    # Body larger than declared CL (extra bytes must be silently ignored)
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 4\r\n\r\n" + b"C" * (_BODY_LIMIT * 3),
    # Large CL with empty body: must time out or reject, not spin
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 9999999\r\n\r\n",
    # Integer overflow variants
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 99999999999999999999\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 18446744073709551615\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 18446744073709551616\r\n\r\n",
]

HEADER_ABUSE = [
    # Oversized values: test header size limits
    b"X-Big: "  + b"A" * 16384  + b"\r\n",
    b"X-Big2: " + b"B" * 65535  + b"\r\n",
    b"X-Big3: " + b"C" * 131072 + b"\r\n",
    # Header count floods
    b"".join(b"X-H-%d: v\r\n" % i for i in range(500)),
    b"".join(b"X-H-%d: v\r\n" % i for i in range(128)),
    b"".join(b"X-H-%d: v\r\n" % i for i in range(70)),
    # Obsolete line folding (obs-fold)
    b"X-Fold: a\r\n b\r\n",
    b"X-Fold2: a\r\n\tb\r\n",
    b"X-Fold3: a\r\n  b\r\n  c\r\n",
    # Bad header names
    b"X-Bad Name: v\r\n",
    b"X-Ws : v\r\n",
    b"X-Tab\tName: v\r\n",
    b": novalue\r\n",
    b"  : v\r\n",
    b"X-" + b"A" * 8192 + b": v\r\n",
    b"X-\x00name: v\r\n",
    b"X-\x01name: v\r\n",
    # Control bytes in values
    b"X-Ctl: va\x01lue\r\n",
    b"X-Nul: va\x00lue\r\n",
    b"X-Bel: va\x07lue\r\n",
    b"X-Del: va\x7flue\r\n",
    b"X-Tab-Val: va\tlue\r\n",
    b"X-NonAscii: caf\xc3\xa9\r\n",
    # Structural
    b"NoColonHeaderLine\r\n",
    b"Host: a\r\nHost: b\r\n",
    b"Host: \r\n",
    # Content-Length attacks
    b"Content-Length: -1\r\n",
    b"Content-Length: -9999\r\n",
    b"Content-Length: 99999999999999999999\r\n",
    b"Content-Length: 18446744073709551615\r\n",
    b"Content-Length: 18446744073709551616\r\n",
    b"Content-Length: 9223372036854775808\r\n",
    b"Content-Length: +5\r\n",
    b"Content-Length: 0x5\r\n",
    b"Content-Length: 1e5\r\n",
    b"Content-Length: 5\r\nContent-Length: 5\r\n",
    b"Content-Length: 5\r\nContent-Length: 6\r\n",
    b"Content-Length:  \r\n",
    b"Content-Length: 5 6\r\n",
    # Transfer-Encoding conflicts
    b"Transfer-Encoding: chunked\r\nContent-Length: 5\r\n",
    b"Transfer-Encoding: chunked\r\nTransfer-Encoding: identity\r\n",
    b"Transfer-Encoding: \r\n",
    # Hop-by-hop / proxy
    b"Connection: keep-alive, X-Secret\r\nX-Secret: injected\r\n",
    b"Proxy-Connection: keep-alive\r\n",
    b"Keep-Alive: timeout=5, max=99999999\r\n",
    b"Upgrade: websocket\r\nConnection: Upgrade\r\n",
    b"X-Forwarded-For: " + b"1.2.3.4, " * 1000 + b"5.6.7.8\r\n",
    b"X-Forwarded-Host: evil.example\r\n",
    b"X-Original-URL: /admin\r\n",
    b"X-Rewrite-URL: /admin\r\n",
    # Bombs
    b"Accept-Encoding: " + b"gzip, " * 2000 + b"identity\r\n",
    b"Accept: " + b"text/html, " * 1000 + b"*/*\r\n",
    b"Cookie: " + b"a=b; " * 2000 + b"c=d\r\n",
    # Empty / whitespace
    b"X-Empty: \r\n",
    b"Content-Type:   \r\n",
    # Range attacks
    b"Range: bytes=0-999999999999\r\n",
    b"Range: bytes=-999999999999\r\n",
    b"Range: bytes=999999999999-0\r\n",
    b"Range: " + b"bytes=0-1, " * 500 + b"bytes=0-1\r\n",
    # Bare CR/LF in values
    b"X-CR: val\rue\r\n",
    b"X-LF: val\nue\r\n",
]

SMUGGLING = [
    # CL.TE
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\nGET /x",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello",
    # TE.CL
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
    # Bad chunk sizes
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\nbad\r\n0\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFF\r\nAAAA",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n-1\r\nAAAA",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n 5\r\nhello\r\n0\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0x5\r\nhello\r\n0\r\n\r\n",
    # TE obfuscation
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: xchunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding:\tchunked\r\n\r\n0\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: CHUNKED\r\n\r\n0\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked, identity\r\n\r\n0\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked \r\n\r\n0\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: identity\r\n\r\n0\r\n\r\n",
    # Bare LF
    b"POST /echo-body HTTP/1.1\nHost: x\nContent-Length: 5\n\nhello",
    b"GET /health HTTP/1.1\nHost: x\n\n",
    # Oversized chunk extension
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5;" + b"a" * 9000 + b"\r\nhello\r\n0\r\n\r\n",
    # Truncated / malformed chunks
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello0\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
    # Pipelined smuggle attempt
    (b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
     b"POST /echo-body HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n" + b"X" * 100),
    # Trailers (most servers silently ignore; verifies no crash)
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nTrailer: X-Evil\r\n\r\n5\r\nhello\r\n0\r\nX-Evil: injected\r\n\r\n",
]

MALFORMED = [
    # Truncated
    b"GET", b"GET /", b"GET / HTTP", b"GET / HTTP/1.1", b"GET / HTTP/1.1\r\n",
    # Bad versions
    b"GET / HTTP/9.9\r\nHost: x\r\n\r\n",
    b"GET / HTTP/1\r\nHost: x\r\n\r\n",
    b"GET / HTTP/1.1.1\r\nHost: x\r\n\r\n",
    b"GET / HTTP/0.9\r\nHost: x\r\n\r\n",
    b"GET /\r\n\r\n",
    b"GET / HTTP/2\r\nHost: x\r\n\r\n",
    b"GET / HTTP/3\r\nHost: x\r\n\r\n",
    b"GET / HTTP/1.1 extra\r\nHost: x\r\n\r\n",
    # Protocol confusion
    b"\x00\x01\x02\x03\xff\xfe garbage\r\n\r\n",
    b"\xff\xfe\x00\x00junk",
    b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n",
    b"\x16\x03\x01\x00\xff" + b"\x00" * 255,
    b"\x80\x01",
    b"EHLO victim.com\r\n",
    b"NICK foo\r\nUSER foo 0 * :foo\r\n",
    b"*1\r\n$4\r\nPING\r\n",
    b"\x00\x00\x00\x0a\x01\x00\x00\x00\x00\x00",
    (b"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
     b"Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
     b"Sec-WebSocket-Version: 13\r\n\r\n"),
    # Null bytes in path
    b"GET /\x00/etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET /foo\x00bar HTTP/1.1\r\nHost: x\r\n\r\n",
    # Whitespace abuse in request line
    b" GET / HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET  /  HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET\t/\tHTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET / HTTP/1.1 \r\nHost: x\r\n\r\n",
    # CRLF flood
    b"GET / HTTP/1.1\r\n" + b"\r\n" * 5000,
    # Overlong URI / Host
    b"GET / HTTP/1.1\r\nHost: " + b"a" * 70000 + b"\r\n\r\n",
    b"GET /" + b"a" * 70000 + b" HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET /" + b"seg/" * 4000 + b" HTTP/1.1\r\nHost: x\r\n\r\n",
    # Method edge cases
    b"G\x00ET / HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET/HTTP/1.1\r\n\r\n",
    b"GET/health HTTP/1.1\r\nHost: x\r\n\r\n",
    # Absolute URI with credentials: SSRF / proxy confusion
    b"GET http://user:pass@127.0.0.1/health HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET http://evil.example/health HTTP/1.1\r\nHost: x\r\n\r\n",
    b"CONNECT evil.example:443 HTTP/1.1\r\nHost: evil.example\r\n\r\n",
    # No Host header
    b"GET / HTTP/1.1\r\nX-No-Host: 1\r\n\r\n",
    # Mixed CRLF/LF
    b"GET / HTTP/1.1\nHost: x\r\nConnection: close\n\r\n",
    # Multiple slashes in path
    b"GET //// HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET //health HTTP/1.1\r\nHost: x\r\n\r\n",
]

METHOD_FORMS = [
    b"OPTIONS * HTTP/1.1\r\nHost: x\r\n\r\n",
    b"TRACE / HTTP/1.1\r\nHost: x\r\n\r\n",
    b"PATCH /health HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n",
    b"PUT /health HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n",
    b"DELETE /health HTTP/1.1\r\nHost: x\r\n\r\n",
    b"HEAD /health HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET http://127.0.0.1/health HTTP/1.1\r\nHost: x\r\n\r\n",
    # Lowercase: method is case-sensitive
    b"get /health HTTP/1.1\r\nHost: x\r\n\r\n",
    b"Get /health HTTP/1.1\r\nHost: x\r\n\r\n",
    # Unknown / custom methods
    b"FOOBARBAZ /health HTTP/1.1\r\nHost: x\r\n\r\n",
    b"1NVALID /health HTTP/1.1\r\nHost: x\r\n\r\n",
    b"G(ET /health HTTP/1.1\r\nHost: x\r\n\r\n",
    (b"M" * 9000) + b" /health HTTP/1.1\r\nHost: x\r\n\r\n",
    b"LOCK /health HTTP/1.1\r\nHost: x\r\n\r\n",
    b"SEARCH /health HTTP/1.1\r\nHost: x\r\n\r\n",
    b"PROPFIND /health HTTP/1.1\r\nHost: x\r\n\r\n",
    b"MKCOL /health HTTP/1.1\r\nHost: x\r\n\r\n",
    # Wrong method for known routes
    b"POST /health HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n",
    b"PUT /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n",
    # HTTP/1.0
    b"GET /health HTTP/1.0\r\n\r\n",
    b"GET /health HTTP/1.0\r\nHost: x\r\n\r\n",
    b"GET /health HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n",
]

VIOLATE_ROUTES = ["/violate/204body", "/violate/conn", "/violate/recommit",
                  "/violate/flush-after-body", "/violate/flush-twice"]
LIGHT_PATHS    = ["/health", "/text", "/api/v1/status", "/chain"]

# Route correctness specification
# Every route in main.cpp is listed here with exact expected behavior
ROUTE_CHECKS = [
    # (method, path, headers, body, expected_status, must_contain_in_body, must_have_header)
    # Basic routes
    ("GET",  "/health",           None, b"", 200, b"ok",     None),
    ("GET",  "/text",             None, b"", 200, b"ok",     None),
    # /echo: echoes X-Echo into body and X-Echoed header
    ("GET",  "/echo",             {"X-Echo": "hello"}, b"", 200, b"hello", ("X-Echoed", b"hello")),
    ("GET",  "/echo",             None, b"", 200, b"(none)", None),
    # /echo-body: echoes POST body
    ("POST", "/echo-body",        None, b"wfx-test", 200, b"wfx-test", None),
    ("POST", "/echo-body",        None, b"",         200, b"",         None),
    # /echo-full: echoes method/path/header/body (small body here; the large-body,
    # buffer-relocation variant is a dedicated check in phase_features)
    ("POST", "/echo-full",        {"X-Marker": "small"}, b"wfx-test", 200, b"wfx-test",
             ("X-Echo-Marker", b"small")),
    # /big: 1MiB of 'A'
    ("GET",  "/big",              None, b"", 200, b"A" * 16, None),
    # /download: requires X-File, returns 400 without it
    ("GET",  "/download",         None, b"", 400, b"missing X-File", None),
    # /metrics: valid JSON with known keys
    ("GET",  "/metrics",          None, b"", 200, b'"crashes"', None),
    # Dynamic segments: uint
    ("GET",  "/items/42",         None, b"", 200, b"42",    None),
    ("GET",  "/items/0",          None, b"", 200, b"0",     None),
    ("GET",  "/items/18446744073709551615", None, b"", 200, b"18446744073709551615", None),
    # Dynamic segments: int (signed)
    ("GET",  "/items/signed/-1",  None, b"", 200, b"-1",   None),
    ("GET",  "/items/signed/-999",None, b"", 200, b"-999", None),
    ("GET",  "/items/signed/0",   None, b"", 200, b"0",    None),
    # Dynamic segments: string
    ("GET",  "/greet/world",      None, b"", 200, b"world", None),
    ("GET",  "/greet/Alice",      None, b"", 200, b"Alice", None),
    ("GET",  "/greet/hello-world",None, b"", 200, b"hello-world", None),
    # Dynamic segments: uuid
    ("GET",  "/uuid/550e8400-e29b-41d4-a716-446655440000", None, b"", 200, b"550e8400", None),
    ("GET",  "/uuid/00000000-0000-0000-0000-000000000001", None, b"", 200, b"00000000", None),
    # Type mismatch: must be 404, not crash
    ("GET",  "/items/notanumber", None, b"", 404, None, None),
    ("GET",  "/uuid/not-a-uuid",  None, b"", 404, None, None),
    ("GET",  "/uuid/12345",       None, b"", 404, None, None),
    # Route groups
    ("GET",  "/api/v1/status",    None, b"", 200, b"ok", None),
    ("GET",  "/api/v1/item/7",    None, b"", 200, b"7",  None),
    ("GET",  "/api/v1/item/42",   None, b"", 200, b"42", None),
    # Middleware: MwBreak: handler must NOT run
    ("GET",  "/mw/blocked",       None, b"", 403, b"blocked", None),
    # Middleware: MwSkipNext: second MW skipped, handler runs
    ("GET",  "/mw/skipnext",      None, b"", 200, b"handler-ran", None),
    # Context storage: middleware sets uid=42, handler echoes it
    ("GET",  "/ctx",              None, b"", 200, b"42", None),
    # Async handler
    ("GET",  "/async/sleep",      None, b"", 200, b"slept", None),
    # JSON: immediate mode
    ("GET",  "/json/im",          None, b"", 200, b'"immediate"', None),
    ("GET",  "/json/im",          None, b"", 200, b'"meta"', None),
    # JSON: retained mode
    ("GET",  "/json/rm",          None, b"", 200, b'"retained"', None),
    # JSON: parse valid
    ("POST", "/parse-json",       {"Content-Type": "application/json"},
             b'{"name":"wfx","version":7}',  200, b'"wfx"',   None),
    ("POST", "/parse-json",       {"Content-Type": "application/json"},
             b'{"name":"chaos","version":42}', 200, b'"chaos"', None),
    # JSON: parse invalid: must return 400
    ("POST", "/parse-json",       {"Content-Type": "application/json"},
             b"not json",   400, None, None),
    ("POST", "/parse-json",       {"Content-Type": "application/json"},
             b'{"broken":', 400, None, None),
    ("POST", "/parse-json",       {"Content-Type": "application/json"},
             b"",            400, None, None),
    # Chained headers
    ("GET",  "/chain",            None, b"", 200, b"chain", None),
    # Templates: static
    ("GET",  "/template/static",  None, b"", 200, b"Hello from WFX Template", None),
    # Templates: dynamic vars
    ("GET",  "/template/dynamic", None, b"", 200, b"WFX Rendered", None),
    ("GET",  "/template/dynamic", None, b"", 200, b"42", None),
    # Templates: if/elif/else branches
    ("GET",  "/template/cond/2",  None, b"", 200, b"high",   None),
    ("GET",  "/template/cond/1",  None, b"", 200, b"medium", None),
    ("GET",  "/template/cond/0",  None, b"", 200, b"low",    None),
    # Templates: for loop
    ("GET",  "/template/loop",    None, b"", 200, b"alpha", None),
    ("GET",  "/template/loop",    None, b"", 200, b"beta",  None),
    ("GET",  "/template/loop",    None, b"", 200, b"gamma", None),
    # Templates: include
    ("GET",  "/template/include", None, b"", 200, b"hello include", None),
    ("GET",  "/template/include", None, b"", 200, b"wfx-footer", None),
    # Templates: extends/block
    ("GET",  "/template/inherit", None, b"", 200, b"My Page",      None),
    ("GET",  "/template/inherit", None, b"", 200, b"Child Content", None),
]

# Helpers
def _probe(host, port, payload, retries=3, delay=0.3):
    """Send payload, retrying while a still-starting worker (post-revival) can't yet serve it.
    raw_send() surfaces that as CONN_ERR (refused), IO_ERR (reset mid-request), or a None status
    (accepted then closed with zero bytes) - retry on all three, not just CONN_ERR, and only
    stop once a real HTTP status code comes back."""
    st, raw = "CONN_ERR", b""
    for i in range(retries):
        st, raw = raw_send(host, port, payload, rtimeout=3.0)
        if isinstance(st, int):
            return st, raw
        if i < retries - 1:
            time.sleep(delay)
    return st, raw

def _fetch_metrics(host, port):
    st, raw = req(host, port, "GET", "/metrics", rtimeout=3.0)
    if st == 200:
        try:
            return json.loads(body_of(raw))
        except Exception:
            pass
    return {}

def _verify_all_routes(host, port):
    """
    Run all ROUTE_CHECKS concurrently.
    Returns (passed, failed, detail_list).
    """
    results = []
    lock = threading.Lock()

    def _check(method, path, hdrs, body, expect_st, needle, hdr_check):
        # /health back up only means the master accepted a connection again, not that every
        # respawned worker slot has finished re-listening yet. Retry until a real HTTP status
        # comes back, same as _probe(), so a route landing on a still-starting worker isn't a
        # false failure - CONN_ERR (refused), IO_ERR (reset), and a None status (accepted then
        # closed with zero bytes) are all the same "not ready yet" signal, not just CONN_ERR
        for attempt in range(3):
            st, raw = req(host, port, method, path, headers=hdrs, body=body,
                          rtimeout=5.0, ctimeout=3.0)
            if isinstance(st, int):
                break
            if attempt < 2:
                time.sleep(0.3)
        b = body_of(raw)
        ok = (st == expect_st)
        if ok and needle is not None:
            ok = needle in b
        if ok and hdr_check is not None:
            _, hdrs_r, _ = parse_hdrs(raw)
            hname, hval = hdr_check
            vals = hdrs_r.get(hname.lower().encode(), [])
            ok = bool(vals) and hval in vals[0]
        detail = None if ok else "%s %s -> %s (expected %s%s)" % (
            method, path, st, expect_st,
            (", body missing %r" % needle) if needle and st == expect_st else "")
        with lock:
            results.append((ok, detail))

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(ROUTE_CHECKS)) as ex:
        futs = [ex.submit(_check, *c) for c in ROUTE_CHECKS]
        concurrent.futures.wait(futs, timeout=20.0)

    passed = sum(1 for ok, _ in results if ok)
    failed = [d for ok, d in results if not ok]
    return passed, len(failed), failed

def _do_kill(mpid, stats, label):
    ws = children(mpid)
    if not ws:
        term.log("chaos", _yellow("  [%s] no workers to kill" % label))
        return None, False
    v = random.choice(ws)
    try:
        os.kill(v, signal.SIGKILL)
        stats["kills"] = stats.get("kills", 0) + 1
        term.log("chaos", "  [%s] SIGKILL PID %d (%d workers)" % (label, v, len(ws)))
        return v, True
    except OSError as e:
        term.log("chaos", _yellow("  [%s] kill failed: %s" % (label, e)))
        return v, False

def _chaos_recover_and_verify(host, port, findings, label, timeout=15.0):
    rec = common.await_health(host, port, timeout)
    if rec is None:
        term.log("chaos", _red("  [%s] server did NOT recover within %.0fs" % (label, timeout)))
        findings.append(("NO_RECOVERY", "[%s] unreachable after kill" % label))
        return False
    term.log("chaos", _green("  [%s] /health back in %.1fs" % (label, rec)))
    p, f, details = _verify_all_routes(host, port)
    if f > 0:
        term.log("chaos", _red("  [%s] %d/%d routes WRONG: %s" % (label, f, p+f, details[:2])))
        findings.append(("CORRECTNESS", "[%s] %d routes wrong after recovery: %s"
                         % (label, f, details[:3])))
    else:
        term.log("chaos", _green("  [%s] %d/%d routes correct" % (label, p, p+f)))
    return True

def _light_flood(host, port, n=8):
    stop = threading.Event()
    def _w():
        while not stop.is_set():
            req(host, port, "GET", random.choice(LIGHT_PATHS), rtimeout=1.0, ctimeout=1.0)
    ts = [threading.Thread(target=_w, daemon=True) for _ in range(n)]
    for t in ts:
        t.start()
    return stop, ts

# PHASE: security
def phase_security(ctx):
    cfg, srv = ctx.cfg, ctx.server
    host, port = cfg.host, cfg.port
    findings = []
    stats = {"trav_url": 0, "trav_hdr": 0, "crlf": 0,
             "violate_ok": 0, "violate_bad": 0, "leak": 0}

    # 16 background threads: representative concurrent load during security probing
    stop_bg = threading.Event()
    def _bg():
        while not stop_bg.is_set():
            req(host, port, "GET", random.choice(LIGHT_PATHS), rtimeout=2.0)
    bg = [threading.Thread(target=_bg, daemon=True) for _ in range(16)]
    for t in bg:
        t.start()

    term.log("security", "started  (%d trav-url  %d trav-hdr  %d CRLF  %d violate)"
         % (len(TRAVERSAL_URL), len(TRAVERSAL_HDR), len(CRLF_VALUES), len(VIOLATE_ROUTES)))

    try:
        # /download without X-File must return 400
        st, _ = _probe(host, port, _build("GET", "/download"))
        if isinstance(st, int) and st != 400:
            findings.append(("MISSING_HEADER_NOT_400",
                              "/download without X-File returned %s, expected 400" % st))

        # URL path traversal
        with term.progress("security", "trav-url", len(TRAVERSAL_URL)) as pr:
            for vec in TRAVERSAL_URL:
                payload = ("GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
                           % vec).encode("latin-1", "replace")
                _, raw = _probe(host, port, payload)
                stats["trav_url"] += 1
                leaked, marker = has_leak(raw)
                if leaked:
                    pr.bad()
                    findings.append(("PATH_TRAVERSAL_URL",
                                      "vec=%r  marker=%r" % (vec, marker)))
                else:
                    pr.ok()

        # X-File header traversal
        with term.progress("security", "trav-hdr", len(TRAVERSAL_HDR)) as pr:
            for vec in TRAVERSAL_HDR:
                _, raw = _probe(host, port, _build("GET", "/download", headers={"X-File": vec}))
                stats["trav_hdr"] += 1
                leaked, marker = has_leak(raw)
                if leaked:
                    pr.bad()
                    findings.append(("PATH_TRAVERSAL_HDR",
                                      "vec=%r  marker=%r" % (vec, marker)))
                else:
                    pr.ok()

        # CRLF / response splitting
        with term.progress("security", "crlf", len(CRLF_VALUES)) as pr:
            for val in CRLF_VALUES:
                payload = (b"GET /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX-Echo: "
                           + val + b"\r\n\r\n")
                _, raw = _probe(host, port, payload)
                stats["crlf"] += 1
                _, hdrs, _ = parse_hdrs(raw)
                if b"x-injected" in hdrs:
                    pr.bad()
                    findings.append(("RESPONSE_SPLIT",
                                      "X-Echo %r caused X-Injected as parsed header name" % val))
                else:
                    pr.ok()

        # Content-Type duplication (real \r\n injection)
        with term.progress("security", "crlf-ct", 2) as pr:
            for val in (b"ok\r\nContent-Type: text/html",
                        b"ok\r\nSet-Cookie: session=evil; Path=/"):
                payload = (b"GET /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX-Echo: "
                           + val + b"\r\n\r\n")
                _, raw = _probe(host, port, payload)
                _, hdrs, _ = parse_hdrs(raw)
                if len(hdrs.get(b"content-type", [])) > 1:
                    pr.bad()
                    findings.append(("RESPONSE_SPLIT",
                                      "Content-Type duplicated by %r" % val))
                else:
                    pr.ok()

        # Double-CRLF fake HTTP response
        term.log("security", "double-CRLF body injection ...")
        dbl = b"safe\r\n\r\nHTTP/1.1 200 Injected\r\nX-Injected: pwned\r\n\r\nbody"
        payload = (b"GET /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX-Echo: "
                   + dbl + b"\r\n\r\n")
        _, raw = _probe(host, port, payload)
        sep = raw.find(b"\r\n\r\n")
        if sep >= 0 and raw[sep+4:].startswith(b"HTTP/1.1 200 Injected"):
            findings.append(("RESPONSE_SPLIT",
                              "double CRLF created parseable second HTTP response"))
            term.log("security", _red("  !! double-CRLF body injection confirmed"))

        # Information leakage on all routes
        # Any route returning sensitive content = critical
        term.log("security", "info leakage check across all routes ...")
        leakable_paths = [
            "/health", "/text", "/metrics", "/json/im", "/json/rm",
            "/api/v1/status", "/chain", "/mw/injected", "/mw/blocked",
            "/ctx", "/async/sleep", "/template/static", "/template/dynamic",
            "/template/loop", "/template/include", "/template/inherit",
        ]
        for path in leakable_paths:
            st, raw = req(host, port, "GET", path, rtimeout=3.0)
            leaked, marker = has_leak(raw)
            if leaked:
                stats["leak"] += 1
                findings.append(("INFO_LEAK",
                                  "route=%s  marker=%r" % (path, marker)))
                term.log("security", _red("  !! LEAK on %s: %r" % (path, marker)))

        # Contract violations: must return 500, never crash
        term.log("security", "contract violations (%d routes x 10 hits) ..." % len(VIOLATE_ROUTES))
        for route in VIOLATE_ROUTES:
            with term.progress("security", route) as pr:
                for _ in range(10):
                    st, _ = _probe(host, port, _build("GET", route))
                    if st == 500:
                        stats["violate_ok"] += 1
                        pr.ok()
                    else:
                        stats["violate_bad"] += 1
                        pr.bad()
                        findings.append(("VIOLATE_BAD_STATUS",
                                          "%s returned %s, expected 500" % (route, st)))

        # Server still alive
        if not common.health(host, port, 4.0):
            findings.append(("SERVER_DEAD", "unreachable after security phase"))

    finally:
        stop_bg.set()

    nf = len(findings)
    term.log("security", _green("all clear") if nf == 0 else _red("%d finding(s)" % nf))

    ctx.phase("security").record(findings, security=("PATH_TRAVERSAL_URL", "PATH_TRAVERSAL_HDR",
                                                     "RESPONSE_SPLIT", "INFO_LEAK"),
                                 vectors=stats["trav_url"] + stats["trav_hdr"] + stats["crlf"]
                                         + stats["leak"] + stats["violate_ok"] + stats["violate_bad"])

# PHASE: protocol
def phase_protocol(ctx):
    cfg, srv = ctx.cfg, ctx.server
    host, port = cfg.host, cfg.port
    findings = []
    sent = 0

    corpora = [
        (HEADER_ABUSE,      "header-abuse"),
        (MALFORMED,         "malformed"),
        (METHOD_FORMS,      "methods"),
        (SMUGGLING,         "smuggling"),
        (BODYBOMB_PAYLOADS, "bodybomb"),
    ]

    term.log("protocol", "started  (%d corpora, %d total vectors, zero concurrent load)"
         % (len(corpora), sum(len(c) for c, _ in corpora)))

    for corpus, label in corpora:
        with term.progress("protocol", label, len(corpus)) as pr:
            for vec in corpus:
                sent += 1
                raw_send(host, port, vec, rtimeout=2.0)
                pr.ok()

            alive_now = common.health(host, port, 5.0)
            pr.finish(_green("alive") if alive_now else _red("DEAD"))

        if not alive_now:
            findings.append(("SERVER_DEAD_AFTER_%s" % label.upper(),
                              "server unreachable after %s" % label))
        else:
            time.sleep(0.5)

    term.log("protocol", "%d vectors: %s" % (sent,
         _green("survived all") if not findings
         else _red("%d corpus killed server" % len(findings))))

    ctx.phase("protocol").record(findings, vectors=sent)

# PHASE: features
def phase_features(ctx):
    cfg, srv = ctx.cfg, ctx.server
    host, port = cfg.host, cfg.port
    findings = []

    # Concurrent background load while checking
    stop_bg = threading.Event()
    def _bg():
        while not stop_bg.is_set():
            req(host, port, "GET", random.choice(LIGHT_PATHS), rtimeout=2.0)
    bg = [threading.Thread(target=_bg, daemon=True) for _ in range(8)]
    for t in bg:
        t.start()

    term.log("features", "started  (8 background threads, %d route checks)" % len(ROUTE_CHECKS))

    try:
        # All routes concurrently
        p, f, details = _verify_all_routes(host, port)
        for d in details:
            findings.append(("ROUTE_FAIL", d))
        term.log("features", "%d/%d routes correct%s" % (p, p+f,
             (": " + _red("%d failed" % f)) if f else ""))

        # Additional invariants

        # mw/injected: middleware must inject X-Route-MW: hit
        with term.progress("features", "mw:header") as pr:
            for _ in range(5):
                st, raw = req(host, port, "GET", "/mw/injected", rtimeout=3.0)
                _, hdrs, _ = parse_hdrs(raw)
                vals = hdrs.get(b"x-route-mw", [])
                if st != 200 or not vals or vals[0] != b"hit":
                    pr.bad()
                    findings.append(("ROUTE_FAIL",
                                      "/mw/injected: X-Route-MW wrong (status=%s vals=%r)" % (st, vals)))
                else:
                    pr.ok()

        # mw/skipnext: skipped MW header must NOT appear
        with term.progress("features", "mw:skipnext") as pr:
            for _ in range(5):
                st, raw = req(host, port, "GET", "/mw/skipnext", rtimeout=3.0)
                _, hdrs, _ = parse_hdrs(raw)
                if b"x-should-not-appear" in hdrs:
                    pr.bad()
                    findings.append(("ROUTE_FAIL", "/mw/skipnext: skipped middleware header appeared"))
                else:
                    pr.ok()

        # template/cond: branch isolation: wrong branches must not appear
        with term.progress("features", "tmpl:cond-isolation") as pr:
            for n, expected, forbidden in [
                (2, b"high",   [b"medium", b"low"]),
                (1, b"medium", [b"high",   b"low"]),
                (0, b"low",    [b"high",   b"medium"]),
            ]:
                st, raw = req(host, port, "GET", "/template/cond/%d" % n, rtimeout=3.0)
                b = body_of(raw)
                if st != 200 or expected not in b:
                    pr.bad()
                    findings.append(("ROUTE_FAIL",
                                      "/template/cond/%d: %r missing" % (n, expected)))
                elif any(x in b for x in forbidden):
                    pr.bad()
                    findings.append(("ROUTE_FAIL",
                                      "/template/cond/%d: forbidden %r present" % (
                                          n, [x for x in forbidden if x in b])))
                else:
                    pr.ok()

        # template/inherit: base title must NOT appear (child overrode it)
        with term.progress("features", "tmpl:inherit") as pr:
            st, raw = req(host, port, "GET", "/template/inherit", rtimeout=3.0)
            b = body_of(raw)
            if b"Base Title" in b:
                pr.bad()
                findings.append(("ROUTE_FAIL",
                                  "/template/inherit: base title leaked: child override failed"))
            else:
                pr.ok()

        # Templates must respond Content-Type: text/html
        with term.progress("features", "tmpl:content-type") as pr:
            for path in ["/template/static", "/template/dynamic", "/template/loop",
                         "/template/include", "/template/inherit"]:
                st, raw = req(host, port, "GET", path, rtimeout=3.0)
                _, hdrs, _ = parse_hdrs(raw)
                ct = b"".join(hdrs.get(b"content-type", [b""]))
                if st != 200 or b"text/html" not in ct:
                    pr.bad()
                    findings.append(("ROUTE_FAIL",
                                      "%s: Content-Type=%r (expected text/html)" % (path, ct)))
                else:
                    pr.ok()

        # /chain: all three headers must be present with exact values
        with term.progress("features", "chain:all-headers") as pr:
            for _ in range(3):
                st, raw = req(host, port, "GET", "/chain", rtimeout=3.0)
                _, hdrs, _ = parse_hdrs(raw)
                ok = (st == 200
                      and hdrs.get(b"x-chain-a", [b""])[0] == b"alpha"
                      and hdrs.get(b"x-chain-b", [b""])[0] == b"beta"
                      and hdrs.get(b"x-chain-c", [b""])[0] == b"gamma")
                if not ok:
                    pr.bad()
                    findings.append(("ROUTE_FAIL",
                                      "/chain: headers wrong (status=%s a=%r b=%r c=%r)" % (
                                          st,
                                          hdrs.get(b"x-chain-a"),
                                          hdrs.get(b"x-chain-b"),
                                          hdrs.get(b"x-chain-c"))))
                else:
                    pr.ok()

        # /async/sleep: 20 concurrent requests: tests timer pool
        term.log("features", "async/sleep x 20 concurrent ...")
        ok20 = fail20 = 0
        lock20 = threading.Lock()
        def _async_req():
            nonlocal ok20, fail20
            st, raw = req(host, port, "GET", "/async/sleep", rtimeout=5.0)
            with lock20:
                if st == 200 and b"slept" in body_of(raw):
                    ok20 += 1
                else:
                    fail20 += 1
        ts = [threading.Thread(target=_async_req) for _ in range(20)]
        for t in ts:
            t.start()
        for t in ts:
            t.join(timeout=10.0)
        if fail20 == 0:
            term.log("features", _green("  async/sleep: 20/20 passed"))
        else:
            term.log("features", _red("  async/sleep: %d/20 failed" % fail20))
            findings.append(("ROUTE_FAIL", "async/sleep concurrent: %d/20 failed" % fail20))

        # /echo-full under forced buffer relocation: request.Path()/GetHeader()/Body() are
        # all string_views into the connection's read buffer - a body big enough to outgrow
        # the initial recv buffer (recv_buffer_incr=8192) forces RWBuffer to relocate
        # mid-parse (see PrepareForBody in http/parser/http_parser.cpp). This test proves
        # every one of those views still points at the right bytes afterward, not just the
        # body - a previous bug here caused false 404s and corrupted routing
        with term.progress("features", "realloc:integrity") as pr:
            marker = "REALLOC-MARK-%d" % random.randint(100000, 999999)
            big_body = b"R" * 50000  # > recv_buffer_incr (8192), < max_body_size (65536)
            payload = _build("POST", "/echo-full", {"X-Marker": marker}, big_body)
            st = raw = None
            for attempt in range(3):
                st, raw = raw_send_dripped(host, port, payload, chunk_size=333, delay=0.002, rtimeout=8.0)
                if isinstance(st, int):
                    break
                if attempt < 2:
                    time.sleep(0.3)
            _, hdrs, b = parse_hdrs(raw)
            ok = (st == 200
                  and hdrs.get(b"x-echo-method", [b""])[0] == b"POST"
                  and hdrs.get(b"x-echo-path", [b""])[0] == b"/echo-full"
                  and hdrs.get(b"x-echo-marker", [b""])[0] == marker.encode()
                  and b == big_body)
            if not ok:
                pr.bad()
                findings.append(("REALLOC_INTEGRITY",
                                  "echo-full mismatch after relocation: status=%s method=%r path=%r "
                                  "marker=%r (expected %r) body_ok=%s body_len=%d (expected %d)"
                                  % (st, hdrs.get(b"x-echo-method"), hdrs.get(b"x-echo-path"),
                                     hdrs.get(b"x-echo-marker"), marker, b == big_body,
                                     len(b), len(big_body))))
            else:
                pr.ok()

        # Boundary correctness: body/header limits from wfx.toml must be enforced exactly
        # at the byte, not off-by-one in either direction (previously only checked for
        # "server survives", never for the actual status code - see phase_protocol)
        with term.progress("features", "limits:boundary") as pr:
            boundary_checks = [
                ("body at limit",     _build("POST", "/echo-body", None, b"B" * _BODY_LIMIT), 200),
                ("body one over",     _build("POST", "/echo-body", None, b"B" * (_BODY_LIMIT + 1)), 400),
                ("header at limit",   _build_at_header_size(_HEADER_LIMIT), 200),
                ("header one over",   _build_at_header_size(_HEADER_LIMIT + 1), 400),
            ]
            for label, vec, expect_st in boundary_checks:
                st, raw = _probe(host, port, vec)
                if st != expect_st:
                    pr.bad()
                    findings.append(("LIMIT_BOUNDARY", "%s: status=%s (expected %s)" % (label, st, expect_st)))
                else:
                    pr.ok()

        # Slow-drip inbound: send a small request one byte at a time, forcing the server
        # to observe it across many separate recv() calls instead of one - exercises the
        # ET-epoll multi-read path independently of buffer relocation (the body here stays
        # well under recv_buffer_incr, so this isolates drip-read correctness from the
        # relocation check above)
        with term.progress("features", "drip:byte-at-a-time") as pr:
            drip_marker = "DRIP-MARK-%d" % random.randint(100000, 999999)
            drip_body = b"d" * 500
            drip_payload = _build("POST", "/echo-full", {"X-Marker": drip_marker}, drip_body)
            st = raw = None
            for attempt in range(3):
                st, raw = raw_send_dripped(host, port, drip_payload, chunk_size=1, rtimeout=10.0)
                if isinstance(st, int):
                    break
                if attempt < 2:
                    time.sleep(0.3)
            _, hdrs, b = parse_hdrs(raw)
            ok = (st == 200
                  and hdrs.get(b"x-echo-marker", [b""])[0] == drip_marker.encode()
                  and b == drip_body)
            if not ok:
                pr.bad()
                findings.append(("DRIP_INTEGRITY",
                                  "echo-full mismatch under byte-at-a-time send: status=%s marker=%r "
                                  "(expected %r) body_ok=%s" % (st, hdrs.get(b"x-echo-marker"), drip_marker, b == drip_body)))
            else:
                pr.ok()

        # /stream: chunked streaming response via res.Stream() (512 chunks x 256 'S'
        # bytes). This whole response-streaming path had no coverage: no ROUTE_CHECK
        # ever requested it. Drive it and verify the full 131072-byte body reassembles
        with term.progress("features", "stream:chunked") as pr:
            st, raw = raw_send(host, port, _build("GET", "/stream"), rtimeout=8.0, rmax=1 << 20)
            sbody = net.dechunk(body_of(raw)) if raw else b""
            expected = b"S" * (512 * 256)
            if st != 200 or sbody != expected:
                pr.bad()
                findings.append(("STREAM_FAIL",
                                  "/stream: status=%s len=%d (expected 200, %d bytes of 'S')"
                                  % (st, len(sbody), len(expected))))
            else:
                pr.ok()

        # /flush/*: awaitable outbound chunk flush, FlushStart()/co_await Flush()/co_await
        # FlushEnd(). Unlike /stream (a generator the engine drives after the handler returns),
        # these send real bytes mid-coroutine, so both wire framing and content need checking.
        with term.progress("features", "flush:single") as pr:
            st, raw = raw_send(host, port, _build("GET", "/flush/single"), rtimeout=8.0)
            rbody = body_of(raw) if raw else b""
            ok = st == 200 and count_chunks(rbody) == 1 and net.dechunk(rbody) == b"single-round-payload"
            (pr.ok() if ok else pr.bad())
            if not ok:
                findings.append(("FLUSH_SINGLE_FAIL", "/flush/single: status=%s chunks=%d body=%r"
                                 % (st, count_chunks(rbody), net.dechunk(rbody))))

        with term.progress("features", "flush:zero") as pr:
            st, raw = raw_send(host, port, _build("GET", "/flush/zero"), rtimeout=8.0)
            rbody = body_of(raw) if raw else b""
            ok = st == 200 and rbody == b"0\r\n\r\n" and net.dechunk(rbody) == b""
            (pr.ok() if ok else pr.bad())
            if not ok:
                findings.append(("FLUSH_ZERO_FAIL", "/flush/zero: status=%s raw_body=%r (expected exactly "
                                 "b'0\\r\\n\\r\\n', a single terminator and nothing else)" % (st, rbody)))

        with term.progress("features", "flush:multi") as pr:
            st, raw = raw_send(host, port, _build("GET", "/flush/multi"), rtimeout=8.0, rmax=1 << 20)
            rbody = body_of(raw) if raw else b""
            expected = ("".join("%d\n" % i for i in range(200))).encode()
            nchunks = count_chunks(rbody)
            ok = st == 200 and nchunks == 200 and net.dechunk(rbody) == expected
            (pr.ok() if ok else pr.bad())
            if not ok:
                findings.append(("FLUSH_MULTI_FAIL", "/flush/multi: status=%s chunks=%d (expected 200) "
                                 "body_ok=%s" % (st, nchunks, net.dechunk(rbody) == expected)))

        # Regression coverage: a handler that returns without calling FlushEnd() used to leave an
        # unbackfilled chunk-header gap (10 raw zero bytes) on the wire, corrupting the framing
        # (curl: "Illegal or missing hexadecimal sequence in chunked-encoding"). Must now still be
        # a validly terminated, if truncated, response, and the connection must survive it.
        with term.progress("features", "flush:no-end") as pr:
            st, raw = raw_send(host, port, _build("GET", "/flush/no-end"), rtimeout=8.0)
            rbody = body_of(raw) if raw else b""
            ok = st == 200 and net.dechunk(rbody) == b"firstsecond"
            healthy = common.health(host, port, 4.0)
            (pr.ok() if (ok and healthy) else pr.bad())
            if not ok:
                findings.append(("FLUSH_NOEND_FAIL", "/flush/no-end: status=%s body=%r (expected "
                                 "b'firstsecond', a valid truncated response)" % (st, net.dechunk(rbody))))
            if not healthy:
                findings.append(("SERVER_DEAD", "server unreachable after /flush/no-end"))

        # 4000 rounds x 2048 bytes (~8 MiB) read back in small dripped pieces: forces the server's
        # non-blocking writes into real EAGAIN, exercising DrainWriteBuffer/ResumeFlushChunk's
        # suspend-and-resume path, not just the synchronous fast path every check above stays on
        with term.progress("features", "flush:heavy(backpressure)") as pr:
            st, raw = raw_recv_dripped(host, port, _build("GET", "/flush/heavy"), rtimeout=30.0)
            rbody = body_of(raw) if raw else b""
            expected = b"".join(bytes([ord('A') + (r % 26)]) * 2048 for r in range(4000))
            ok = st == 200 and net.dechunk(rbody) == expected
            (pr.ok() if ok else pr.bad())
            if not ok:
                findings.append(("FLUSH_HEAVY_FAIL", "/flush/heavy: status=%s len=%d (expected 200, "
                                 "%d bytes)" % (st, len(net.dechunk(rbody)), len(expected))))

        if not common.health(host, port, 4.0):
            findings.append(("SERVER_DEAD", "server unreachable after features phase"))

    finally:
        stop_bg.set()

    nf = len(findings)
    term.log("features", _green("all clear") if nf == 0 else _red("%d failure(s)" % nf))
    ctx.phase("features").record(findings, vectors=len(ROUTE_CHECKS))

def _form(host, port, path, content_type, body, rtimeout=3.0):
    hdrs = {} if content_type is None else {"Content-Type": content_type}
    return req(host, port, "POST", path, headers=hdrs, body=body, rtimeout=rtimeout)

def _form_json(host, port, path, content_type, body, rtimeout=3.0):
    st, raw = _form(host, port, path, content_type, body, rtimeout=rtimeout)
    try:
        return st, json.loads(body_of(raw))
    except Exception:
        return st, None

# PHASE: forms
def phase_forms(ctx):
    cfg, srv = ctx.cfg, ctx.server
    host, port = cfg.host, cfg.port
    findings = []
    checks = 0
    CT = "application/x-www-form-urlencoded"

    def check(name, cond, detail="", tag="FORM_FAIL"):
        nonlocal checks
        checks += 1

        if cond:
            pr.ok()
        else:
            pr.bad()
            findings.append((tag, "%s: %s" % (name, detail)))

    VALID_BODY = b"username=alice&email=alice%40example.com&age=30&bio=hi"

    term.log("forms", "started")
    with term.progress("forms", "functional") as pr:

        # Happy path
        st, j = _form_json(host, port, "/form", CT, VALID_BODY)
        check("valid submission", st == 200 and j and j.get("username") == "alice"
              and j.get("email") == "alice@example.com" and j.get("age") == 30
              and j.get("bio_present") is True and j.get("bio_len") == 2,
              "status=%s body=%r" % (st, j))

        # Content-Type matching
        for name, ct, want in [
            ("wrong type json", "application/json", 415),
            ("wrong type multipart", "multipart/form-data; boundary=x", 415),
            ("missing content-type", None, 415),
            ("uppercase type", CT.upper(), 200),
            ("type with charset param", CT + "; charset=utf-8", 200),
            ("type trailing space", CT + " ", 200),
            ("prefix-junk suffix", CT + "x", 415),
        ]:
            st, _ = _form(host, port, "/form", ct, VALID_BODY)
            check("content-type: %s" % name, st == want, "got %s want %s" % (st, want))

        # Structural parsing (field order / count / equals)
        for name, body in [
            ("missing field", b"username=alice&email=alice%40example.com&age=30"),
            ("extra field", b"username=alice&email=alice%40example.com&age=30&bio=hi&extra=1"),
            ("wrong order", b"email=alice%40example.com&username=alice&age=30&bio=hi"),
            ("no equals sign", b"username&email=alice%40example.com&age=30&bio=hi"),
            ("trailing ampersand", VALID_BODY + b"&"),
            ("empty body", b""),
            ("duplicate key wrong slot", b"username=alice&username=bob&age=30&bio=hi"),
        ]:
            st, _ = _form(host, port, "/form", CT, body)
            check("structure: %s" % name, st == 400, "got %s, want 400" % st)

        # Required-but-empty vs optional-missing
        st, _ = _form(host, port, "/form", CT, b"username=&email=alice%40example.com&age=30&bio=hi")
        check("required empty -> 422", st == 422, "got %s" % st)

        # Validator boundaries
        def with_username(u):
            return ("username=%s&email=alice%%40example.com&age=30&bio=hi" % u).encode()

        for name, u, want in [
            ("username len 3 (min)", "abc", 200),
            ("username len 2 (under min)", "ab", 422),
            ("username len 32 (max)", "a" * 32, 200),
            ("username len 33 (over max)", "a" * 33, 422),
        ]:
            st, _ = _form(host, port, "/form", CT, with_username(u))
            check("boundary: %s" % name, st == want, "got %s want %s" % (st, want))

        def with_age(a):
            return ("username=alice&email=alice%%40example.com&age=%s&bio=hi" % a).encode()

        for name, a, want in [
            ("age 0 (min)", "0", 200), ("age 120 (max)", "120", 200),
            ("age 121 (over)", "121", 422), ("age negative", "-1", 422),
            ("age non-numeric", "abc", 422), ("age overflow u64", "99999999999999999999", 422),
        ]:
            st, _ = _form(host, port, "/form", CT, with_age(a))
            check("boundary: %s" % name, st == want, "got %s want %s" % (st, want))

        def with_email(e):
            return ("username=alice&email=%s&age=30&bio=hi" % e).encode()

        for name, e, want in [
            ("email valid", "a%40b.co", 200), ("email no at", "nope", 422),
            ("email trailing at", "a%40", 422), ("email leading at", "%40b.com", 422),
            ("email double dot local", "a..b%40x.com", 422),
            ("email double dot domain", "a%40b..com", 422),
            ("email no dot domain", "a%40b", 422),
        ]:
            st, _ = _form(host, port, "/form", CT, with_email(e))
            check("boundary: %s" % name, st == want, "got %s want %s" % (st, want))

    # Percent-decoding fuzz (via /form/raw, single isolated field)
    with term.progress("forms", "percent-decoding") as pr:

        def raw_decode(v):
            return _form_json(host, port, "/form/raw", CT, b"v=" + v)

        st, j = raw_decode(b"%00")
        check("decode NUL byte", st == 200 and j and j.get("len") == 1, "st=%s j=%r" % (st, j))

        st, j = raw_decode(b"%25")
        check("decode %25 -> literal percent", st == 200 and j and j.get("len") == 1
              and j.get("value") == "%", "st=%s j=%r" % (st, j))

        st, j = raw_decode(b"%2500")
        check("no double-decode (%2500)", st == 200 and j and j.get("len") == 3
              and j.get("value") == "%00", "st=%s j=%r (would be len=1 if double-decoded)" % (st, j),
              tag="FORM_DECODE_BYPASS")

        st, j = raw_decode(b"%")
        check("lone trailing percent -> literal", st == 200 and j and j.get("value") == "%", "st=%s j=%r" % (st, j))

        st, j = raw_decode(b"%4")
        check("one hex digit then end -> literal", st == 200 and j and j.get("value") == "%4", "st=%s j=%r" % (st, j))

        st, j = raw_decode(b"%2")
        check("%2 at exact end -> literal", st == 200 and j and j.get("value") == "%2", "st=%s j=%r" % (st, j))

        for bad in (b"%ZZ", b"%2G", b"%G2", b"%-1", b"%0G", b"%G0"):
            st, _ = raw_decode(bad)
            check("invalid hex escape rejected: %r" % bad, st == 400, "got %s" % st, tag="FORM_DECODE_BYPASS")

        st, j = raw_decode(b"a+b")
        check("+ decodes to space", st == 200 and j and j.get("value") == "a b", "st=%s j=%r" % (st, j))

        st, j = raw_decode(b"a%2Bb")
        check("encoded plus stays literal", st == 200 and j and j.get("value") == "a+b", "st=%s j=%r" % (st, j))

        st, j = raw_decode(b"a" * 8192)
        check("value at max (8192)", st == 200 and j and j.get("len") == 8192, "st=%s len=%s" % (st, j and j.get("len")))

        st, j = raw_decode(b"a" * 8193)
        check("value over max (8193)", st == 422, "got %s" % st)

        # Raw (pre-decode) body is 3x larger than the decoded result: validator must
        # bound the DECODED length, not the raw wire length
        st, j = raw_decode(b"%61" * 8192)
        check("validated post-decode, not pre-decode", st == 200 and j and j.get("len") == 8192,
              "st=%s len=%s" % (st, j and j.get("len")))

    # Decoded-value-into-header injection (response splitting)
    with term.progress("forms", "header-injection") as pr:

        for name, u in [
            ("crlf + header", "AAA%0d%0aX-Injected%3a%20evil"),
            ("bare cr", "AAA%0dX-Injected%3a%20evil"),
            ("bare lf", "AAA%0aX-Injected%3a%20evil"),
            ("double crlf + line", "AAA%0d%0a%0d%0aGET%20%2fevil%20HTTP%2f1.1"),
        ]:
            st, raw = _form(host, port, "/form", CT, with_username(u))
            _, hdrs, _ = parse_hdrs(raw)
            leaked = b"x-injected" in hdrs
            check("no header split: %s" % name, not leaked, "raw=%r" % raw[:200],
                  tag="FORM_HEADER_INJECT")

    # Structural DoS / hang resistance
    with term.progress("forms", "dos-resistance") as pr:

        t0 = time.time()
        many = b"&".join(b"k%d=v" % i for i in range(200))
        st, _ = _form(host, port, "/form", CT, many, rtimeout=5.0)
        dt = time.time() - t0
        check("200 pairs rejected promptly", st == 400 and dt < 3.0, "status=%s elapsed=%.2fs" % (st, dt))

        t0 = time.time()
        st, _ = _form(host, port, "/form", CT, b"&" * 500, rtimeout=5.0)
        dt = time.time() - t0
        check("500 empty segments rejected promptly", st == 400 and dt < 3.0, "status=%s elapsed=%.2fs" % (st, dt))

    if not common.health(host, port, 4.0):
        findings.append(("SERVER_DEAD", "server unreachable after forms phase"))

    nf = len(findings)
    term.log("forms", _green("all clear") if nf == 0 else _red("%d finding(s)" % nf))

    ctx.phase("forms").record(findings, security=("FORM_HEADER_INJECT", "FORM_DECODE_BYPASS"),
                              vectors=checks)

def _query_json(host, port, path, rtimeout=3.0):
    st, raw = req(host, port, "GET", path, rtimeout=rtimeout)
    try:
        return st, json.loads(body_of(raw))
    except Exception:
        return st, None

# PHASE: query
def phase_query(ctx):
    cfg, srv = ctx.cfg, ctx.server
    host, port = cfg.host, cfg.port
    findings = []
    checks = 0

    def check(name, cond, detail="", tag="QUERY_FAIL"):
        nonlocal checks
        checks += 1

        if cond:
            pr.ok()
        else:
            pr.bad()
            findings.append((tag, "%s: %s" % (name, detail)))

    term.log("query", "started")
    with term.progress("query", "functional") as pr:

        for name, path, want_present, want_value, want_count in [
            ("simple present",         "/query/echo?v=hello",        True,  "hello", 1),
            ("no query string at all", "/query/echo",                False, "",      0),
            ("empty query string",     "/query/echo?",               False, "",      0),
            ("key absent",             "/query/echo?x=1",            False, "",      1),
            ("value found among many","/query/echo?a=1&v=target&b=2",True, "target",3),
            ("empty value",            "/query/echo?v=",             True,  "",      1),
            ("bare key, no equals",    "/query/echo?v",               True,  "",      1),
            ("duplicate keys: first wins", "/query/echo?v=first&v=second", True, "first", 2),
            ("substring key must not match", "/query/echo?av=99",    False, "",      1),
            ("leading/trailing empty segments", "/query/echo?&v=1&", True,  "1",     1),
            ("all-empty segments",     "/query/echo?&&&",             False, "",      0),
        ]:
            st, j = _query_json(host, port, path)
            ok = (st == 200 and j is not None and j.get("present") == want_present
                  and j.get("count") == want_count
                  and (not want_present or j.get("value") == want_value))
            check(name, ok, "status=%s body=%r" % (st, j))

    # Regression coverage for the path/query split fix: a query value shaped like a path must
    # survive completely untouched, not get collapsed/resolved like a real path would
    with term.progress("query", "traversal-shaped values") as pr:
        for name, raw_value in [
            ("parent-dir segment",  "/foo/../bar"),
            ("double slash",        "//evil"),
            ("bare double dot",     ".."),
            ("bare single dot",     "."),
            ("dot-slash prefix",    "./x"),
            ("many collapsing slashes", "a////////b"),
            ("mixed traversal",     "../../../etc/passwd"),
        ]:
            st, j = _query_json(host, port, "/query/echo?v=" + raw_value)
            ok = st == 200 and j is not None and j.get("present") is True and j.get("value") == raw_value
            check("survives untouched: %s" % name, ok, "status=%s body=%r (want value=%r)" % (st, j, raw_value),
                  tag="QUERY_TRAVERSAL_MANGLED")

    # No auto-decoding: WFX doesn't decode query values for you, same as it doesn't decode
    # header values, so '+' and '%XX' must come back exactly as they arrived on the wire
    with term.progress("query", "no-auto-decode") as pr:
        st, j = _query_json(host, port, "/query/echo?v=a+b")
        check("'+' stays literal, not decoded to space", st == 200 and j and j.get("value") == "a+b",
              "st=%s j=%r" % (st, j))

        st, j = _query_json(host, port, "/query/echo?v=%20")
        check("'%20' stays literal, not decoded to space", st == 200 and j and j.get("value") == "%20",
              "st=%s j=%r" % (st, j))

    # Path normalization must still work normally when a query string follows it
    with term.progress("query", "path+query interaction") as pr:
        st, raw = req(host, port, "GET", "/items//42?v=1")
        b = body_of(raw)
        check("double-slash path still normalizes with a query attached", st == 200 and b == b"42",
              "status=%s body=%r" % (st, b))

        st, raw = req(host, port, "GET", "/items/1/../42?v=1")
        b = body_of(raw)
        check("dot-segment path still resolves with a query attached", st == 200 and b == b"42",
              "status=%s body=%r" % (st, b))

    # Buffer growth/relocation: header_reserve_hint is 512 bytes, recv_buffer_incr is 8192, so a
    # query value comfortably past 512 but under max_header_size (8192) forces at least one grow
    # cycle. This is what actually exercises the parser fix's memmove reassembly, not just the
    # split-then-normalize logic on a small request that never needed to grow
    with term.progress("query", "buffer growth") as pr:
        big_value = "x" * 4000
        st, j = _query_json(host, port, "/query/echo?pad=" + ("p" * 1000) + "&v=" + big_value)
        check("large query value survives buffer growth", st == 200 and j and j.get("present") is True
              and j.get("len") == 4000 and j.get("value") == big_value,
              "st=%s len=%s" % (st, j and j.get("len")))

        # Same growth scenario, checked through the full raw path this time (not just the parsed
        # value), via the existing /echo-full route's X-Echo-Path header
        traversal_value = "/a/../b" * 200  # ~1400 bytes, still traversal-shaped, still must survive
        st, raw = req(host, port, "POST", "/echo-full?v=" + traversal_value,
                      headers={"X-Marker": "grow"}, body=b"body", rtimeout=5.0)
        _, hdrs, _ = parse_hdrs(raw)
        echoed_path = b"".join(hdrs.get(b"x-echo-path", [b""])).decode(errors="replace")
        check("full path+query survives growth via X-Echo-Path",
              st == 200 and echoed_path == "/echo-full?v=" + traversal_value,
              "status=%s echoed_path=%r" % (st, echoed_path[:120]))

    # Many pairs: correctness and basic responsiveness, not a hard DoS bound like forms' equivalent
    # check (there's no per-request pair-count cap here), just confirms a large-but-legal query
    # string still parses correctly and promptly
    with term.progress("query", "many-pairs") as pr:
        pairs = "&".join("k%d=v%d" % (i, i) for i in range(100))
        t0 = time.time()
        st, j = _query_json(host, port, "/query/echo?" + pairs + "&v=findme", rtimeout=5.0)
        dt = time.time() - t0
        check("100 pairs parsed correctly and promptly",
              st == 200 and j and j.get("present") is True and j.get("value") == "findme"
              and j.get("count") == 101 and dt < 3.0,
              "st=%s j=%r elapsed=%.2fs" % (st, j, dt))

    if not common.health(host, port, 4.0):
        findings.append(("SERVER_DEAD", "server unreachable after query phase"))

    nf = len(findings)
    term.log("query", _green("all clear") if nf == 0 else _red("%d finding(s)" % nf))

    ctx.phase("query").record(findings, security=("QUERY_TRAVERSAL_MANGLED",), vectors=checks)

def _cors_headers(host, port, method, path, headers=None, rtimeout=3.0):
    st, raw = req(host, port, method, path, headers=headers, rtimeout=rtimeout)
    _, hdrs, body = parse_hdrs(raw)
    return st, hdrs, body

def _h(hdrs, name):
    vals = hdrs.get(name.lower().encode())
    return vals[0].decode(errors="replace") if vals else None

# PHASE: cors
def phase_cors(ctx):
    cfg = ctx.cfg
    host, port = cfg.host, cfg.port
    findings = []
    checks = 0

    def check(name, cond, detail="", tag="CORS_FAIL"):
        nonlocal checks
        checks += 1

        if cond:
            pr.ok()
        else:
            pr.bad()
            findings.append((tag, "%s: %s" % (name, detail)))

    term.log("cors", "started")

    # Simple (non-preflight) requests: the matched origin is echoed back exactly, every other
    # config-driven header appears, and the handler's own headers still work normally alongside it
    with term.progress("cors", "simple request, matched origin") as pr:
        for origin in ("https://allowed.example", "https://second.example"):
            st, hdrs, body = _cors_headers(host, port, "GET", "/health", headers={"Origin": origin})
            check("origin echoed exactly: %s" % origin,
                  st == 200 and _h(hdrs, "access-control-allow-origin") == origin,
                  "status=%s aco=%r" % (st, _h(hdrs, "access-control-allow-origin")))
            check("credentials true: %s" % origin,
                  _h(hdrs, "access-control-allow-credentials") == "true",
                  "%r" % _h(hdrs, "access-control-allow-credentials"))
            check("vary: origin present: %s" % origin, _h(hdrs, "vary") == "Origin", "%r" % _h(hdrs, "vary"))
            check("expose-headers static list: %s" % origin,
                  _h(hdrs, "access-control-expose-headers") == "X-Exposed-Data",
                  "%r" % _h(hdrs, "access-control-expose-headers"))
            check("preflight-only headers absent on a simple request: %s" % origin,
                  _h(hdrs, "access-control-allow-methods") is None
                  and _h(hdrs, "access-control-allow-headers") is None
                  and _h(hdrs, "access-control-max-age") is None,
                  "methods=%r headers=%r max-age=%r" % (_h(hdrs, "access-control-allow-methods"),
                                                        _h(hdrs, "access-control-allow-headers"),
                                                        _h(hdrs, "access-control-max-age")))
            check("handler's own headers untouched: %s" % origin,
                  body == b"ok" and _h(hdrs, "content-type") == "text/plain",
                  "body=%r content-type=%r" % (body, _h(hdrs, "content-type")))

    # Basic origin reflection (real vuln class): an origin NOT on the allowlist must get nothing,
    # not an echo, and the request itself must still succeed normally, CORS is enforced by the
    # browser, the server just chooses whether to hand out the headers that let it read the result
    with term.progress("cors", "unmatched origin rejected") as pr:
        for name, origin in [
            ("attacker domain",                   "https://evil.attacker.example"),
            ("null origin (CVE-2019-9580 class)",  "null"),
            ("suffix bypass",                      "https://allowed.example.evil.com"),
            ("prefix bypass",                      "https://evilallowed.example"),
            ("scheme mismatch",                    "http://allowed.example"),
            ("port mismatch",                      "https://allowed.example:8443"),
            ("case mismatch",                      "https://ALLOWED.EXAMPLE"),
        ]:
            st, hdrs, body = _cors_headers(host, port, "GET", "/health", headers={"Origin": origin})
            no_cors = (_h(hdrs, "access-control-allow-origin") is None
                       and _h(hdrs, "access-control-allow-credentials") is None
                       and _h(hdrs, "vary") is None
                       and _h(hdrs, "access-control-expose-headers") is None)
            check("no CORS headers leak: %s (%s)" % (name, origin), st == 200 and no_cors and body == b"ok",
                  "status=%s body=%r aco=%r acac=%r" % (st, body, _h(hdrs, "access-control-allow-origin"),
                                                        _h(hdrs, "access-control-allow-credentials")),
                  tag="CORS_ORIGIN_LEAK")

    # No Origin header at all: a non-browser client (curl, server-to-server) is unaffected
    with term.progress("cors", "no origin header") as pr:
        st, hdrs, body = _cors_headers(host, port, "GET", "/health")
        check("plain request unaffected",
              st == 200 and body == b"ok" and _h(hdrs, "access-control-allow-origin") is None,
              "status=%s body=%r" % (st, body))

    # Persistent headers survive AbortWithError: matched-origin CORS headers must still be on a
    # 404, not just successful responses, this is the entire reason WritePersistentHeader exists
    with term.progress("cors", "persistent headers survive 404") as pr:
        st, hdrs, body = _cors_headers(host, port, "GET", "/definitely/not/a/route",
                                       headers={"Origin": "https://allowed.example"})
        check("CORS headers present on a 404",
              st == 404 and _h(hdrs, "access-control-allow-origin") == "https://allowed.example"
              and _h(hdrs, "access-control-allow-credentials") == "true"
              and _h(hdrs, "vary") == "Origin",
              "status=%s aco=%r" % (st, _h(hdrs, "access-control-allow-origin")))

    # Real preflight: Origin + Access-Control-Request-Method both present
    with term.progress("cors", "preflight, matched origin") as pr:
        st, hdrs, body = _cors_headers(host, port, "OPTIONS", "/health", headers={
            "Origin": "https://allowed.example",
            "Access-Control-Request-Method": "PUT",
            "Access-Control-Request-Headers": "X-Custom-Header",
        })
        check("204 no body", st == 204 and body == b"", "status=%s body=%r" % (st, body))
        check("allow-methods is the static configured list",
              _h(hdrs, "access-control-allow-methods") == "GET, POST, PUT, PATCH, DELETE, OPTIONS",
              "%r" % _h(hdrs, "access-control-allow-methods"))
        check("allow-headers reflects the request (allowed_headers is empty in config)",
              _h(hdrs, "access-control-allow-headers") == "X-Custom-Header",
              "%r" % _h(hdrs, "access-control-allow-headers"))
        check("max-age matches config", _h(hdrs, "access-control-max-age") == "300",
              "%r" % _h(hdrs, "access-control-max-age"))
        check("origin/credentials/vary/expose still present on preflight",
              _h(hdrs, "access-control-allow-origin") == "https://allowed.example"
              and _h(hdrs, "access-control-allow-credentials") == "true"
              and _h(hdrs, "vary") == "Origin"
              and _h(hdrs, "access-control-expose-headers") == "X-Exposed-Data",
              "aco=%r acac=%r vary=%r expose=%r" % (_h(hdrs, "access-control-allow-origin"),
                                                    _h(hdrs, "access-control-allow-credentials"),
                                                    _h(hdrs, "vary"), _h(hdrs, "access-control-expose-headers")))

        # A preflight for a completely unregistered path still gets a full answer: CORS validates
        # the origin's cross-origin intent, not whether the eventual real request has anywhere to
        # land, matches how Flask-CORS/Express cors both behave
        st2, hdrs2, _ = _cors_headers(host, port, "OPTIONS", "/no/such/route/at/all", headers={
            "Origin": "https://allowed.example",
            "Access-Control-Request-Method": "GET",
        })
        check("preflight answered even for a path with no real route",
              st2 == 204 and _h(hdrs2, "access-control-allow-origin") == "https://allowed.example",
              "status=%s aco=%r" % (st2, _h(hdrs2, "access-control-allow-origin")))

    # Preflight omitting Access-Control-Request-Headers: the allow-headers response header must
    # be omitted entirely, not sent empty
    with term.progress("cors", "preflight without requested headers") as pr:
        st, hdrs, _ = _cors_headers(host, port, "OPTIONS", "/health", headers={
            "Origin": "https://allowed.example",
            "Access-Control-Request-Method": "GET",
        })
        check("allow-headers omitted, not empty",
              st == 204 and _h(hdrs, "access-control-allow-headers") is None,
              "%r" % _h(hdrs, "access-control-allow-headers"))

    # Preflight-shaped request from an origin NOT on the allowlist: must not be answered as CORS
    # at all. It still falls through to the generic OPTIONS/Allow: fallback since /health has a
    # real GET route, but it must carry zero CORS headers
    with term.progress("cors", "preflight-shaped request, unmatched origin") as pr:
        st, hdrs, _ = _cors_headers(host, port, "OPTIONS", "/health", headers={
            "Origin": "https://evil.attacker.example",
            "Access-Control-Request-Method": "GET",
        })
        check("falls through to generic Allow, no CORS headers",
              st == 204 and _h(hdrs, "allow") == "GET"
              and _h(hdrs, "access-control-allow-origin") is None
              and _h(hdrs, "access-control-allow-methods") is None,
              "status=%s allow=%r aco=%r" % (st, _h(hdrs, "allow"), _h(hdrs, "access-control-allow-origin")),
              tag="CORS_ORIGIN_LEAK")

    # Generic OPTIONS (CoreEngine::HandleGenericOptions): no Origin at all, just a bare capability
    # probe against a path with several real methods and no explicit OPTIONS handler
    with term.progress("cors", "generic OPTIONS, no CORS involved") as pr:
        st, hdrs, body = _cors_headers(host, port, "OPTIONS", "/cors/multi")
        check("Allow lists exactly the registered methods",
              st == 204 and _h(hdrs, "allow") == "GET, POST, PUT" and body == b"",
              "status=%s allow=%r body=%r" % (st, _h(hdrs, "allow"), body))
        check("no CORS headers on a plain capability probe",
              _h(hdrs, "access-control-allow-origin") is None,
              "%r" % _h(hdrs, "access-control-allow-origin"))

        # A real preflight on the same path uses the CORS-configured method list, not the route's
        # own registered methods, the two mechanisms are deliberately independent
        st2, hdrs2, _ = _cors_headers(host, port, "OPTIONS", "/cors/multi", headers={
            "Origin": "https://allowed.example",
            "Access-Control-Request-Method": "POST",
        })
        check("preflight on the same path uses the CORS config list, not the route's methods",
              st2 == 204 and _h(hdrs2, "access-control-allow-methods") == "GET, POST, PUT, PATCH, DELETE, OPTIONS",
              "%r" % _h(hdrs2, "access-control-allow-methods"))

    # OPTIONS on a path with no route under any method at all: plain 404 either way
    with term.progress("cors", "OPTIONS on a truly nonexistent path") as pr:
        st, hdrs, _ = _cors_headers(host, port, "OPTIONS", "/no/route/for/any/method")
        check("plain 404, no Allow header", st == 404 and _h(hdrs, "allow") is None,
              "status=%s allow=%r" % (st, _h(hdrs, "allow")))

    # Oversized bogus Origin: must fail the set lookup cleanly, no crash, no partial match
    with term.progress("cors", "oversized origin value") as pr:
        big_origin = "https://" + ("x" * 2000) + ".example"
        st, hdrs, body = _cors_headers(host, port, "GET", "/health", headers={"Origin": big_origin},
                                       rtimeout=4.0)
        check("large unmatched origin handled cleanly",
              st == 200 and body == b"ok" and _h(hdrs, "access-control-allow-origin") is None,
              "status=%s body_len=%d" % (st, len(body)))

    if not common.health(host, port, 4.0):
        findings.append(("SERVER_DEAD", "server unreachable after cors phase"))

    nf = len(findings)
    term.log("cors", _green("all clear") if nf == 0 else _red("%d finding(s)" % nf))

    ctx.phase("cors").record(findings, security=("CORS_ORIGIN_LEAK",), vectors=checks)

def _find_route(metrics, pred):
    for r in metrics.get("routes", []):
        if pred(r):
            return r
    return None

def _route_exact(method, path):
    return lambda r: r.get("method") == method and r.get("path") == path

def _route_prefix(method, prefix):
    return lambda r: r.get("method") == method and (r.get("path") or "").startswith(prefix)

# PHASE: metrics
def phase_metrics(ctx):
    cfg = ctx.cfg
    host, port = cfg.host, cfg.port
    p = ctx.phase("metrics")

    before = _fetch_metrics(host, port)
    p.check("metrics: latency histograms enabled", before.get("latency_enabled") is True,
            "expected [Metrics] latency = true in wfx.toml, got %r" % before.get("latency_enabled"))

    # Identity plumbing: two known routes each show up with their own path and method.
    # /status/<code:uint> is the status-class workhorse, /health a second static route
    sb = _find_route(before, _route_prefix("GET", "/status"))
    hb = _find_route(before, _route_exact("GET", "/health"))
    p.check("metrics: dynamic route identity (path + method) attached", sb is not None,
            "no GET /status* route in routes[]: %r" % [r.get("path") for r in before.get("routes", [])])
    p.check("metrics: static route identity (path + method) attached", hb is not None,
            "GET /health absent")
    if sb is None or hb is None:
        return

    # Drive every status class through the one /status route, and /health separately
    counts = {200: 20, 301: 7, 404: 11, 500: 5}
    for code, n in counts.items():
        for _ in range(n):
            req(host, port, "GET", "/status/%d" % code, rtimeout=3.0)
    n_health = 13
    for _ in range(n_health):
        req(host, port, "GET", "/health", rtimeout=3.0)

    after = _fetch_metrics(host, port)
    sa = _find_route(after, _route_prefix("GET", "/status"))
    ha = _find_route(after, _route_exact("GET", "/health"))
    if sa is None or ha is None:
        p.failed("metrics: routes vanished after drive", "status=%r health=%r" % (sa, ha))
        return

    n_status_total = sum(counts.values())
    checks = [
        ("requests count matches total driven", sa["requests"] - sb["requests"], n_status_total),
        ("2xx status class matches",            sa["status_2xx"] - sb["status_2xx"], counts[200]),
        ("3xx status class matches",            sa["status_3xx"] - sb["status_3xx"], counts[301]),
        ("4xx status class matches",            sa["status_4xx"] - sb["status_4xx"], counts[404]),
        ("5xx status class matches",            sa["status_5xx"] - sb["status_5xx"], counts[500]),
    ]
    for label, got, want in checks:
        p.check("metrics: " + label, got == want, "expected +%d, got +%d" % (want, got))

    p.check("metrics: 1xx bucket stays zero (no final-1xx over HTTP)",
            sa["status_1xx"] - sb["status_1xx"] == 0)
    p.check("metrics: bytes_out increased (one byte body x N)",
            sa["bytes_out"] - sb["bytes_out"] >= n_status_total)

    # Route isolation: /health traffic lands only on /health, never on /status
    p.check("metrics: per-route isolation (health counted on its own slot)",
            ha["requests"] - hb["requests"] == n_health,
            "expected +%d on /health, got +%d" % (n_health, ha["requests"] - hb["requests"]))
    p.check("metrics: /health traffic never bleeds into /status requests",
            (sa["requests"] - sb["requests"]) == n_status_total)

    # Latency: one sample per served request on each route's own histogram
    ls_b, ls_a = sb.get("latency") or {}, sa.get("latency") or {}
    lh_b, lh_a = hb.get("latency") or {}, ha.get("latency") or {}
    p.check("metrics: /status latency count tracks its requests",
            ls_a.get("count", 0) - ls_b.get("count", 0) == n_status_total,
            "expected +%d, got +%d" % (n_status_total, ls_a.get("count", 0) - ls_b.get("count", 0)))
    p.check("metrics: /health latency count tracks its requests",
            lh_a.get("count", 0) - lh_b.get("count", 0) == n_health,
            "expected +%d, got +%d" % (n_health, lh_a.get("count", 0) - lh_b.get("count", 0)))
    p.check("metrics: latency percentiles ordered and populated",
            ls_a.get("count", 0) == 0 or
            (ls_a.get("p50_us", 0) > 0 and ls_a.get("p99_us", 0) >= ls_a.get("p50_us", 0)
             and ls_a.get("max_us", 0) >= ls_a.get("p99_us", 0)),
            "p50=%r p99=%r max=%r" % (ls_a.get("p50_us"), ls_a.get("p99_us"), ls_a.get("max_us")))
    p.check("metrics: latency mean within [p50-ish, max]",
            ls_a.get("count", 0) == 0 or (0 < ls_a.get("mean_us", 0) <= ls_a.get("max_us", 1)),
            "mean=%r max=%r" % (ls_a.get("mean_us"), ls_a.get("max_us")))

    if not ctx.server.alive():
        p.failed("SERVER_DEAD", "unreachable after metrics phase")

# PHASE: chaos
def phase_chaos(ctx):
    cfg, srv = ctx.cfg, ctx.server
    host, port = cfg.host, cfg.port
    findings = []
    stats    = {}

    mpid = srv.pid()
    if not mpid:
        term.log("chaos", _yellow("no master PID: skipped"))
        return

    term.log("chaos", "started  (master PID=%d)" % mpid)

    # 1. Single worker kills x 3
    term.log("chaos", term.bold("1/6: single worker kill x 3"))
    for i in range(3):
        stop, ts = _light_flood(host, port, 8)
        _do_kill(mpid, stats, "kill-%d" % (i+1))
        ok = _chaos_recover_and_verify(host, port, findings, "kill-%d" % (i+1))
        stop.set()
        for t in ts:
            t.join(timeout=3.0)
        if not ok:
            break
        time.sleep(0.5)

    # 2. Kills under sustained load (30s, 1 kill/5s)
    term.log("chaos", term.bold("2/6: kills under sustained load (30s)"))
    stop, ts = _light_flood(host, port, 16)
    t0 = time.time()
    kn = 0
    while time.time() - t0 < 30.0:
        time.sleep(5.0)
        _, ok = _do_kill(mpid, stats, "load-%d" % (kn+1))
        if ok:
            kn += 1
    stop.set()
    for t in ts:
        t.join(timeout=5.0)
    _chaos_recover_and_verify(host, port, findings, "load-kills")
    term.log("chaos", "  %d kills under load" % kn)

    # 3. Rapid-fire kills (1 kill/2s x 5)
    term.log("chaos", term.bold("3/6: rapid-fire kills (1/2s x 5)"))
    stop, ts = _light_flood(host, port, 12)
    rk = 0
    for i in range(5):
        time.sleep(2.0)
        _, ok = _do_kill(mpid, stats, "rapid-%d" % (i+1))
        if ok:
            rk += 1
    stop.set()
    for t in ts:
        t.join(timeout=3.0)
    _chaos_recover_and_verify(host, port, findings, "rapid-fire", timeout=20.0)
    term.log("chaos", "  %d rapid kills" % rk)

    # 4. Kill both workers simultaneously
    term.log("chaos", term.bold("4/6: simultaneous dual-worker kill"))
    ws = children(mpid)
    if len(ws) >= 2:
        for w in ws[:2]:
            try:
                os.kill(w, signal.SIGKILL)
                stats["kills"] = stats.get("kills", 0) + 1
                term.log("chaos", "  dual-kill PID %d" % w)
            except OSError as e:
                term.log("chaos", _yellow("  dual-kill failed: %s" % e))
        _chaos_recover_and_verify(host, port, findings, "dual-kill", timeout=20.0)
    else:
        term.log("chaos", _yellow("  only %d worker: skipping dual kill" % len(ws)))

    # 5. SIGSTOP -> hammer 3s -> SIGCONT
    term.log("chaos", term.bold("5/6: SIGSTOP worker for 3s then SIGCONT"))
    ws = children(mpid)
    if ws:
        v = random.choice(ws)
        try:
            os.kill(v, signal.SIGSTOP)
            stats["stops"] = stats.get("stops", 0) + 1
            term.log("chaos", "  SIGSTOP PID %d" % v)
            stop, ts = _light_flood(host, port, 8)
            time.sleep(3.0)
            stop.set()
            for t in ts:
                t.join(timeout=4.0)
            os.kill(v, signal.SIGCONT)
            term.log("chaos", "  SIGCONT PID %d" % v)
            p, f, details = _verify_all_routes(host, port)
            if f > 0:
                findings.append(("SIGSTOP_FAIL",
                                  "%d routes wrong after SIGCONT: %s" % (f, details[:2])))
                term.log("chaos", _red("  %d routes wrong after SIGCONT" % f))
            else:
                term.log("chaos", _green("  all routes OK after SIGCONT"))
        except OSError as e:
            term.log("chaos", _yellow("  SIGSTOP/SIGCONT failed: %s" % e))
    else:
        term.log("chaos", _yellow("  no workers for SIGSTOP"))

    # 6. Kill under mixed load: idle connections + concurrent large responses
    #
    # 75 half-open connections sit in read-wait on the server (no audit threads,
    # just held FDs). 12 threads each pull a 1MiB /big response in a loop
    # 12 is enough to saturate the server's send path; more would just thrash
    # Python's scheduler on a 2-core CI runner without adding real server stress
    # After 2s of active load a worker is killed. This exercises epoll cleanup of
    # in-progress sends AND idle-fd teardown in a single scenario
    term.log("chaos", term.bold("6/6: kill under mixed load (75 idle conns + 12 /big threads)"))

    held = []
    for _ in range(75):
        try:
            s = socket.create_connection((host, port), timeout=1.0)
            s.settimeout(1.0)
            s.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n")
            held.append(s)
        except OSError:
            break
    stats["conn_held"] = len(held)
    term.log("chaos", "  holding %d idle connections" % len(held))

    big_ok = big_fail = 0
    big_lock = threading.Lock()
    stop_big = threading.Event()
    def _big_req():
        nonlocal big_ok, big_fail
        while not stop_big.is_set():
            st, _ = req(host, port, "GET", "/big", rtimeout=8.0, ctimeout=2.0)
            with big_lock:
                if st == 200:
                    big_ok += 1
                else:
                    big_fail += 1
    bts = [threading.Thread(target=_big_req, daemon=True) for _ in range(12)]
    for t in bts:
        t.start()
    time.sleep(2.0)

    _do_kill(mpid, stats, "mixed-load")

    stop_big.set()
    for t in bts:
        t.join(timeout=10.0)
    for s in held:
        try:
            s.close()
        except OSError:
            pass
    held.clear()

    term.log("chaos", "  /big: ok=%d fail=%d  idle-conns=%d" % (big_ok, big_fail, stats["conn_held"]))
    _chaos_recover_and_verify(host, port, findings, "mixed-load")

    time.sleep(1.0)
    if not common.health(host, port, 5.0):
        findings.append(("FINAL_DEAD", "server unreachable at end of chaos phase"))

    fm = _fetch_metrics(host, port)
    nf = len(findings)
    msg = "all scenarios survived" if nf == 0 else "%d issue(s)" % nf
    term.log("chaos", "%s  (kills=%d  crashes=%s  restarts=%s)"
         % ((_green(msg) if nf == 0 else _red(msg)),
            stats.get("kills", 0),
            fm.get("process", {}).get("crashes", "?"),
            fm.get("process", {}).get("restarts", "?")))

    ctx.phase("chaos").record(findings, vectors=stats.get("kills", 0) + stats.get("stops", 0))

def _read_http_response(sock, buf):
    """Read exactly one CL-framed HTTP response off `sock`. Returns (status, body, leftover)."""
    while b"\r\n\r\n" not in buf:
        try:
            d = sock.recv(65536)
        except OSError:
            return None, b"", buf
        if not d:
            return None, b"", buf
        buf += d

    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    try:
        status = int(lines[0].split(b" ")[1])
    except (IndexError, ValueError):
        return None, b"", rest

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

    return status, rest[:clen], rest[clen:]

# PHASE: soak
def phase_soak(ctx):
    cfg = ctx.cfg
    host, port = cfg.host, cfg.port
    findings = []

    # CL-framed routes with mixed body sizes so the read buffer sees varying request
    # shapes and the write buffer varying response lengths across reuse
    rotation = [
        ("/health",        200, b"ok"),
        ("/text",          200, b"ok"),
        ("/items/42",      200, b"42"),
        ("/greet/soak",    200, b"hello soak"),
        ("/api/v1/status", 200, b"ok"),
    ]
    n = 2000
    term.log("soak", "started  (%d sequential requests on ONE keep-alive connection)" % n)

    try:
        sock = socket.create_connection((host, port), timeout=5.0)
        sock.settimeout(5.0)
    except OSError as e:
        ctx.phase("soak").record([("SOAK_CONNECT", "could not open keep-alive socket: %s" % e)])
        return

    buf = b""
    completed = 0
    with term.progress("soak", "keep-alive", n) as pr:
        try:
            for i in range(n):
                path, exp_st, exp_body = rotation[i % len(rotation)]
                reqb = ("GET %s HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n" % path).encode("latin-1")
                try:
                    sock.sendall(reqb)
                except OSError as e:
                    findings.append(("SOAK_SEND", "send failed at request #%d: %s" % (i, e)))
                    pr.bad()
                    break

                st, rbody, buf = _read_http_response(sock, buf)
                if st != exp_st or rbody != exp_body:
                    findings.append(("SOAK_MISFRAME",
                                      "request #%d %s -> status=%r body=%r (expected %d / %r)"
                                      % (i, path, st, rbody[:64], exp_st, exp_body)))
                    pr.bad()
                    break

                completed += 1
                pr.ok()
        finally:
            try:
                sock.close()
            except OSError:
                pass

    term.log("soak", "%d/%d requests correct on one connection" % (completed, n))
    if completed < n and not findings:
        findings.append(("SOAK_INCOMPLETE", "only %d/%d completed on the reused connection" % (completed, n)))

    if not common.health(host, port, 4.0):
        findings.append(("SERVER_DEAD", "server unreachable after soak phase"))

    ctx.phase("soak").record(findings, vectors=completed)

# Torture suite: hostile load and deliberate worker kills, so the interesting output is what went
# wrong rather than a pass count. Phases collect findings and flush them with Phase.record()
#
# Two behaviours it needs beyond the default lifecycle:
#   - stop on a security finding, since later phases would be measuring an already-compromised
#     server and their results would be noise
#   - a per-phase timeout, because a phase that wedges under load would otherwise stall the run
class BaseAudit(common.Suite):
    name = "base_audit"
    description = "WFX server torture audit: security, protocol, features, forms, chaos"
    phases = {
        "security": phase_security,
        "protocol": phase_protocol,
        "features": phase_features,
        "forms":    phase_forms,
        "query":    phase_query,
        "cors":     phase_cors,
        "metrics":  phase_metrics,
        "soak":     phase_soak,
        "chaos":    phase_chaos,
    }

    stop_on_security = True
    confirm_exit = True   # the chaos phase kills workers, so confirm the master really exited
    heartbeat = 30        # some phases run silent for minutes and CI kills a quiet job

    def add_arguments(self, parser):
        parser.add_argument("--pid-file", default=None, metavar="PATH",
                            help="daemon pid file, needed by the chaos phase to signal workers "
                                 "(default: ~/.wfx/daemons/<app>.pid)")
        parser.add_argument("--phase-timeout", type=int, default=0, metavar="S",
                            help="max seconds per phase before marking TIMEOUT and continuing "
                                 "(0 = unlimited, 300 under --ci)")
        parser.set_defaults(ready_timeout=20, wfx_logs=common.logs.CRASH)

    def configure(self, cfg):
        cfg.pid_file = cfg.args.pid_file

        # A phase wedged under load costs the whole CI job otherwise
        self.phase_timeout = cfg.args.phase_timeout or (300 if term.is_ci() else 0)

    def before_phases(self, ctx):
        # Read once the daemon is up: the pid file does not exist before that
        master = ctx.server.pid()
        if master:
            term.log("runner", "master PID=%d" % master)
        else:
            term.log("runner", _yellow("no master PID: chaos phase limited"))

if __name__ == "__main__":
    common.run(BaseAudit)
