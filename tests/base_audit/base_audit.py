#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX test harness.
#
# Phases:
#   security   path traversal, CRLF/response-splitting, info leakage,
#              header attacks, contract violations
#   protocol   malformed and abusive request vectors — server must survive without crashing
#   features   every route verified: correct status, body, headers
#   chaos      worker kills, SIGSTOP/SIGCONT, kill under mixed load
#
# Usage:
#   python3 harness.py                   # all phases
#   python3 harness.py --phase security
#   python3 harness.py --list-phases
#
# Exit codes:
#   0   all phases passed
#   1   crash / hang / correctness failure / server death
#   2   security finding (traversal, response splitting, info leak)

import argparse
import collections
import concurrent.futures
import json
import os
import random
import signal
import socket
import subprocess
import sys
import threading
import time


# ─────────────────────────────────────────────────────────────────────────────
# Terminal
# ─────────────────────────────────────────────────────────────────────────────

_TTY = sys.stdout.isatty()
_CI  = False  # set via --ci; enables GitHub Actions workflow commands

def _c(code, t): return ("\x1b[%sm%s\x1b[0m" % (code, t)) if _TTY else t
def _green(t):   return _c("32", t)
def _red(t):     return _c("31", t)
def _yellow(t):  return _c("33", t)
def _cyan(t):    return _c("36", t)
def _mag(t):     return _c("35", t)
def _bold(t):    return _c("1",  t)

def _log(tag, msg="", c=None):
    ts = time.strftime("%H:%M:%S")
    print("[%s] %s  %s" % (ts, (c or _cyan)("%-10s" % tag), msg), flush=True)

def _dot(ch=".", col="32"):
    if _CI: return
    sys.stdout.write(_c(col, ch)); sys.stdout.flush()

def _progress_hdr(text):
    """Inline progress prefix. TTY: no newline. CI: full log line."""
    if _CI:
        print(text.rstrip(), flush=True)
    else:
        sys.stdout.write(text); sys.stdout.flush()

def _progress_end(suffix=""):
    """End a dot-progress line. TTY: newline. CI: print suffix if any."""
    if _CI:
        if suffix: print("  " + suffix, flush=True)
    else:
        print((" " + suffix) if suffix else "", flush=True)

def _gh_group(name):
    if _CI: print("::group::" + name, flush=True)

def _gh_endgroup():
    if _CI: print("::endgroup::", flush=True)

def _gh_error(msg):
    if _CI: print("::error::" + msg, flush=True)

def _gh_warning(msg):
    if _CI: print("::warning::" + msg, flush=True)

def _hdr(title, w=78):
    print("═" * w); print("  " + _bold(title)); print("═" * w)

def _sec(name, verdict, w=78):
    print("\n  %s  [%s]" % (_bold("── " + name), verdict))
    print("─" * w)


# ─────────────────────────────────────────────────────────────────────────────
# Raw HTTP — stdlib socket only, no external deps
# ─────────────────────────────────────────────────────────────────────────────

def raw_send(host, port, payload, rtimeout=3.0, rmax=1<<20, ctimeout=4.0):
    """Send raw bytes, read until close/timeout. Never raises. Returns (status, raw)."""
    try:
        s = socket.create_connection((host, port), timeout=ctimeout)
    except OSError:
        return "CONN_ERR", b""
    try:
        s.sendall(payload)
        s.settimeout(rtimeout)
        buf, total = [], 0
        while total < rmax:
            try:
                d = s.recv(65536)
            except (socket.timeout, OSError):
                break
            if not d: break
            buf.append(d); total += len(d)
        raw = b"".join(buf)
        return _status(raw), raw
    except OSError:
        return "IO_ERR", b""
    finally:
        try: s.close()
        except OSError: pass

def _status(raw):
    if not raw.startswith(b"HTTP/"): return None
    try:    return int(raw.split(b" ", 2)[1])
    except: return None

def _build(method, path, headers=None, body=b"", close=True):
    lines = ["%s %s HTTP/1.1" % (method, path), "Host: x"]
    if close:    lines.append("Connection: close")
    if headers:
        for k, v in headers.items(): lines.append("%s: %s" % (k, v))
    if body:     lines.append("Content-Length: %d" % len(body))
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body

def req(host, port, method, path, headers=None, body=b"", **kw):
    return raw_send(host, port, _build(method, path, headers, body), **kw)

def body_of(raw):
    i = raw.find(b"\r\n\r\n")
    return raw[i+4:] if i >= 0 else b""

def parse_hdrs(raw):
    i = raw.find(b"\r\n\r\n")
    if i < 0: return None, {}, raw
    head = raw[:i]; body = raw[i+4:]
    lines = head.split(b"\r\n")
    hdrs = {}
    for line in lines[1:]:
        if b":" not in line: continue
        k, _, v = line.partition(b":")
        hdrs.setdefault(k.strip().lower(), []).append(v.strip())
    return lines[0] if lines else None, hdrs, body

def health(host, port, timeout=2.0):
    st, _ = req(host, port, "GET", "/health", rtimeout=timeout, ctimeout=timeout)
    return st == 200

def wait_up(host, port, timeout=20.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if health(host, port, timeout=1.0): return time.time() - t0
        time.sleep(0.3)
    return None


# ─────────────────────────────────────────────────────────────────────────────
# /proc
# ─────────────────────────────────────────────────────────────────────────────

_HAS_PROC = os.path.isdir("/proc")

def children(ppid):
    if not _HAS_PROC: return []
    out = []
    try:
        for e in os.listdir("/proc"):
            if not e.isdigit(): continue
            try:
                with open("/proc/%s/status" % e) as f:
                    for line in f:
                        if line.startswith("PPid:"):
                            if int(line.split()[1]) == ppid: out.append(int(e))
                            break
            except (OSError, ValueError): pass
    except OSError: pass
    return out

def rss_mb(pids):
    if not _HAS_PROC: return 0.0
    total = 0
    for p in pids:
        try:
            with open("/proc/%d/status" % p) as f:
                for line in f:
                    if line.startswith("VmRSS:"): total += int(line.split()[1]); break
        except (OSError, ValueError): pass
    return total / 1024.0

def fd_count(pids):
    if not _HAS_PROC: return 0
    total = 0
    for p in pids:
        try: total += len(os.listdir("/proc/%d/fd" % p))
        except OSError: pass
    return total

def proc_alive(pid):
    try: os.kill(pid, 0); return True
    except OSError: return False


# ─────────────────────────────────────────────────────────────────────────────
# Server context
# ─────────────────────────────────────────────────────────────────────────────

class Server:
    def __init__(self, cfg):
        self.cfg  = cfg
        self._pid = None
        self._up  = False

    def start(self):
        cmd = [self.cfg.wfx, "run", self.cfg.app_dir,
               "--port", str(self.cfg.port), "--detach"]
        _log("server", "starting: %s" % " ".join(cmd))
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("wfx run failed (rc=%d): %s %s"
                               % (r.returncode, r.stdout.strip(), r.stderr.strip()))
        self._up = True
        _log("server", _green("detached OK"))

    def wait_ready(self):
        _log("server", "waiting for /health …")
        t = wait_up(self.cfg.host, self.cfg.port, self.cfg.ready_timeout)
        if t is None:
            raise RuntimeError("server at %s:%d never responded within %ds"
                               % (self.cfg.host, self.cfg.port, self.cfg.ready_timeout))
        _log("server", _green("up in %.1fs" % t))

    def pid(self):
        if self._pid: return self._pid
        if not self.cfg.pid_file: return None
        try:
            txt = open(os.path.expanduser(self.cfg.pid_file)).read().strip()
            try:
                d = json.loads(txt)
                self._pid = int(d.get("pid") or d.get("Pid") or 0) or None
            except Exception:
                for line in txt.splitlines():
                    if "=" in line:
                        k, _, v = line.partition("=")
                        if k.strip() == "pid":
                            try: self._pid = int(v.strip()) or None
                            except ValueError: pass
                if not self._pid:
                    try: self._pid = int(txt) or None
                    except ValueError: pass
        except (OSError, ValueError): pass
        return self._pid

    def is_alive(self):
        return health(self.cfg.host, self.cfg.port, timeout=2.0)

    def stop(self):
        if not self._up: return
        _log("server", "stopping …")
        subprocess.run([self.cfg.wfx, "control", "stop", "app"],
                       capture_output=True, text=True)
        mpid = self.pid()
        if mpid:
            deadline = time.time() + 10.0
            while time.time() < deadline:
                if not proc_alive(mpid):
                    _log("server", _green("stopped (PID %d exited)" % mpid))
                    return
                time.sleep(0.25)
            _log("server", _yellow("PID %d still alive — SIGKILL" % mpid))
            try: os.kill(mpid, signal.SIGKILL)
            except OSError: pass
        else:
            _log("server", _green("stopped"))


# ─────────────────────────────────────────────────────────────────────────────
# Payload corpora
# ─────────────────────────────────────────────────────────────────────────────

# Leak markers — any of these in a response = information leakage finding.
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
        if a in raw and (b is None or b in raw): return True, a
    return False, None


def _build_traversal_url():
    targets = [
        "etc/passwd", "etc/shadow", "etc/crontab", "etc/hosts",
        "etc/sudoers", "etc/group", "etc/os-release",
        "proc/self/environ", "proc/self/cmdline", "proc/self/maps", "proc/self/fd/0",
        "app/wfx.toml", "app/src/main.cpp",
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
    seen = set(); out = []
    for v in vecs:
        if v not in seen: seen.add(v); out.append(v)
    return out


def _build_traversal_hdr():
    targets = [
        "etc/passwd", "etc/shadow", "etc/crontab", "etc/hosts",
        "etc/sudoers", "etc/group", "etc/os-release",
        "proc/self/environ", "proc/self/cmdline", "proc/self/maps", "proc/self/fd/0",
        "app/wfx.toml", "app/src/main.cpp",
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
    seen = set(); out = []
    for v in vecs:
        if v not in seen: seen.add(v); out.append(v)
    return out


TRAVERSAL_URL = _build_traversal_url()
TRAVERSAL_HDR = _build_traversal_hdr()

# CRLF corpus: check is STRICT — only flags if the marker appears as a parsed
# HEADER NAME (split on \r\n, partitioned at first :). Substrings inside
# values never count. Zero false positives.
CRLF_VALUES = [
    # Real CRLF injection — the only ones that actually split in HTTP/1.1
    b"safe\r\nX-Injected: pwned",
    b"safe\r\n\r\nX-Injected: pwned",
    b"safe\r\nX-Injected: pwned\r\nX-Extra: yes",
    b"safe\r\nContent-Type: text/html",
    b"safe\r\nSet-Cookie: session=evil; Path=/; HttpOnly",
    b"safe\r\nLocation: http://evil.example/",
    b"safe\r\nTransfer-Encoding: chunked",
    b"safe\r\nContent-Length: 0",
    # Bare LF — some parsers accept as line terminator
    b"safe\nX-Injected: pwned",
    # Null + CRLF — null-terminated string bypass
    b"safe\x00\r\nX-Injected: pwned",
    # Double CRLF — response body injection
    b"safe\r\n\r\nHTTP/1.1 200 Injected\r\nX-Injected: pwned\r\n\r\nbody",
    # Non-splitters (verify no false positive):
    b"safe\rX-Injected: pwned",          # bare CR is NOT a header terminator
    b"safe\tX-Injected: pwned",          # tab is NOT a line terminator
    b"\x00X-Injected: pwned",            # null alone is NOT
    b"safe\xe2\x80\xa8X-Injected: pwned",  # U+2028 NOT
    b"safe\xe2\x80\xa9X-Injected: pwned",  # U+2029 NOT
    # Long values — no injection, just stress the header buffer
    b"A" * 8192,
    b"A" * 16384,
    b"A" * 32768,
]

# Max body size from wfx.toml = 65536.
# Test at-limit, one-over, and integer-overflow variants.
_BODY_LIMIT = 65536
BODYBOMB_PAYLOADS = [
    # Exactly at limit — should be accepted (200)
    _build("POST", "/echo-body", body=b"B" * _BODY_LIMIT),
    # One byte over — must be rejected cleanly (400), not crash
    _build("POST", "/echo-body", body=b"B" * (_BODY_LIMIT + 1)),
    # Moderately over
    _build("POST", "/echo-body", body=b"B" * (_BODY_LIMIT * 2)),
    # Body larger than declared CL (extra bytes must be silently ignored)
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 4\r\n\r\n" + b"C" * (_BODY_LIMIT * 3),
    # Large CL with empty body — must time out or reject, not spin
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 9999999\r\n\r\n",
    # Integer overflow variants
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 99999999999999999999\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 18446744073709551615\r\n\r\n",
    b"POST /echo-body HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 18446744073709551616\r\n\r\n",
]

HEADER_ABUSE = [
    # Oversized values — test header size limits
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
    # Absolute URI with credentials — SSRF / proxy confusion
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
    # Lowercase — method is case-sensitive
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

VIOLATE_ROUTES = ["/violate/204body", "/violate/conn", "/violate/recommit"]
LIGHT_PATHS    = ["/health", "/text", "/api/v1/status", "/chain"]


# ─────────────────────────────────────────────────────────────────────────────
# Route correctness specification
# Every route in main.cpp is listed here with exact expected behavior.
# ─────────────────────────────────────────────────────────────────────────────

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
    # /big: 1MiB of 'A'
    ("GET",  "/big",              None, b"", 200, b"A" * 16, None),
    # /download: requires X-File, returns 400 without it
    ("GET",  "/download",         None, b"", 400, b"missing X-File", None),
    # /metrics: valid JSON with known keys
    ("GET",  "/metrics",          None, b"", 200, b'"crashes"', None),
    # Dynamic segments — uint
    ("GET",  "/items/42",         None, b"", 200, b"42",    None),
    ("GET",  "/items/0",          None, b"", 200, b"0",     None),
    ("GET",  "/items/18446744073709551615", None, b"", 200, b"18446744073709551615", None),
    # Dynamic segments — int (signed)
    ("GET",  "/items/signed/-1",  None, b"", 200, b"-1",   None),
    ("GET",  "/items/signed/-999",None, b"", 200, b"-999", None),
    ("GET",  "/items/signed/0",   None, b"", 200, b"0",    None),
    # Dynamic segments — string
    ("GET",  "/greet/world",      None, b"", 200, b"world", None),
    ("GET",  "/greet/Alice",      None, b"", 200, b"Alice", None),
    ("GET",  "/greet/hello-world",None, b"", 200, b"hello-world", None),
    # Dynamic segments — uuid
    ("GET",  "/uuid/550e8400-e29b-41d4-a716-446655440000", None, b"", 200, b"550e8400", None),
    ("GET",  "/uuid/00000000-0000-0000-0000-000000000001", None, b"", 200, b"00000000", None),
    # Type mismatch — must be 404, not crash
    ("GET",  "/items/notanumber", None, b"", 404, None, None),
    ("GET",  "/uuid/not-a-uuid",  None, b"", 404, None, None),
    ("GET",  "/uuid/12345",       None, b"", 404, None, None),
    # Route groups
    ("GET",  "/api/v1/status",    None, b"", 200, b"ok", None),
    ("GET",  "/api/v1/item/7",    None, b"", 200, b"7",  None),
    ("GET",  "/api/v1/item/42",   None, b"", 200, b"42", None),
    # Middleware: MwBreak — handler must NOT run
    ("GET",  "/mw/blocked",       None, b"", 403, b"blocked", None),
    # Middleware: MwSkipNext — second MW skipped, handler runs
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
    # JSON: parse invalid — must return 400
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

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def _probe(host, port, payload, retries=3, delay=0.3):
    """Send payload with retries on CONN_ERR only."""
    for i in range(retries):
        st, raw = raw_send(host, port, payload, rtimeout=3.0)
        if st != "CONN_ERR": return st, raw
        if i < retries - 1: time.sleep(delay)
    return "CONN_ERR", b""

def _fetch_metrics(host, port):
    st, raw = req(host, port, "GET", "/metrics", rtimeout=3.0)
    if st == 200:
        try: return json.loads(body_of(raw))
        except Exception: pass
    return {}

class _Heartbeat:
    """Logs a 'still running' line every `interval` seconds. CI only."""
    def __init__(self, interval=30):
        self._stop  = threading.Event()
        self._phase = "?"
        self._t0    = time.time()
        self._lock  = threading.Lock()
        self._t     = threading.Thread(target=self._run, daemon=True, name="heartbeat")
        self._t.start()

    def set_phase(self, name):
        with self._lock:
            self._phase = name
            self._t0    = time.time()

    def _run(self):
        while not self._stop.wait(30):
            with self._lock:
                phase   = self._phase
                elapsed = time.time() - self._t0
            _log("harness", "still running — phase=%s  elapsed=%.0fs" % (phase, elapsed))

    def stop(self):
        self._stop.set()
        self._t.join(timeout=5.0)


def _run_phase_timed(name, fn, cfg, srv, timeout):
    """Run phase fn in a thread; if it exceeds timeout mark TIMEOUT and return."""
    result = [None]
    def _worker(): result[0] = fn(cfg, srv)
    t = threading.Thread(target=_worker, daemon=True, name="phase-%s" % name)
    t.start()
    t.join(timeout=timeout)
    if t.is_alive():
        msg = "phase %s exceeded %ds limit" % (name, timeout)
        _gh_error(msg)
        _log("harness", _red("TIMEOUT — " + msg))
        return {"name": name, "rc": 1,
                "findings": [("TIMEOUT", msg)], "stats": {}}
    return result[0]


def _wait_recovery(host, port, timeout=15.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if health(host, port, timeout=1.0): return time.time() - t0
        time.sleep(0.3)
    return None

def _verify_all_routes(host, port):
    """
    Run all ROUTE_CHECKS concurrently.
    Returns (passed, failed, detail_list).
    """
    results = []
    lock = threading.Lock()

    def _check(method, path, hdrs, body, expect_st, needle, hdr_check):
        st, raw = req(host, port, method, path, headers=hdrs, body=body,
                      rtimeout=5.0, ctimeout=3.0)
        b = body_of(raw)
        ok = (st == expect_st)
        if ok and needle is not None: ok = needle in b
        if ok and hdr_check is not None:
            _, hdrs_r, _ = parse_hdrs(raw)
            hname, hval = hdr_check
            vals = hdrs_r.get(hname.lower().encode(), [])
            ok = bool(vals) and hval in vals[0]
        detail = None if ok else "%s %s → %s (expected %s%s)" % (
            method, path, st, expect_st,
            (", body missing %r" % needle) if needle and st == expect_st else "")
        with lock: results.append((ok, detail))

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(ROUTE_CHECKS)) as ex:
        futs = [ex.submit(_check, *c) for c in ROUTE_CHECKS]
        concurrent.futures.wait(futs, timeout=20.0)

    passed = sum(1 for ok, _ in results if ok)
    failed = [d for ok, d in results if not ok]
    return passed, len(failed), failed

def _do_kill(mpid, stats, label):
    ws = children(mpid)
    if not ws:
        _log("chaos", _yellow("  [%s] no workers to kill" % label))
        return None, False
    v = random.choice(ws)
    try:
        os.kill(v, signal.SIGKILL)
        stats["kills"] = stats.get("kills", 0) + 1
        _log("chaos", "  [%s] SIGKILL PID %d (%d workers)" % (label, v, len(ws)))
        return v, True
    except OSError as e:
        _log("chaos", _yellow("  [%s] kill failed: %s" % (label, e)))
        return v, False

def _chaos_recover_and_verify(host, port, findings, label, timeout=15.0):
    rec = _wait_recovery(host, port, timeout)
    if rec is None:
        _log("chaos", _red("  [%s] server did NOT recover within %.0fs" % (label, timeout)))
        findings.append(("NO_RECOVERY", "[%s] unreachable after kill" % label))
        return False
    _log("chaos", _green("  [%s] /health back in %.1fs" % (label, rec)))
    p, f, details = _verify_all_routes(host, port)
    if f > 0:
        _log("chaos", _red("  [%s] %d/%d routes WRONG: %s" % (label, f, p+f, details[:2])))
        findings.append(("CORRECTNESS", "[%s] %d routes wrong after recovery: %s"
                         % (label, f, details[:3])))
    else:
        _log("chaos", _green("  [%s] %d/%d routes correct" % (label, p, p+f)))
    return True

def _light_flood(host, port, n=8):
    stop = threading.Event()
    def _w():
        while not stop.is_set():
            req(host, port, "GET", random.choice(LIGHT_PATHS), rtimeout=1.0, ctimeout=1.0)
    ts = [threading.Thread(target=_w, daemon=True) for _ in range(n)]
    for t in ts: t.start()
    return stop, ts


# ─────────────────────────────────────────────────────────────────────────────
# Phase 1 — SECURITY
# ─────────────────────────────────────────────────────────────────────────────

def phase_security(cfg, srv):
    host, port = cfg.host, cfg.port
    findings = []
    stats = {"trav_url": 0, "trav_hdr": 0, "crlf": 0,
             "violate_ok": 0, "violate_bad": 0, "leak": 0}

    # 16 background threads — representative concurrent load during security probing
    stop_bg = threading.Event()
    def _bg():
        while not stop_bg.is_set():
            req(host, port, "GET", random.choice(LIGHT_PATHS), rtimeout=2.0)
    bg = [threading.Thread(target=_bg, daemon=True) for _ in range(16)]
    for t in bg: t.start()

    _log("security", "started  (%d trav-url  %d trav-hdr  %d CRLF  %d violate)"
         % (len(TRAVERSAL_URL), len(TRAVERSAL_HDR), len(CRLF_VALUES), len(VIOLATE_ROUTES)))

    try:
        # ── /download without X-File must return 400 ─────────────────────────
        st, _ = _probe(host, port, _build("GET", "/download"))
        if st not in (400, "CONN_ERR", None):
            findings.append(("MISSING_HEADER_NOT_400",
                              "/download without X-File returned %s, expected 400" % st))

        # ── URL path traversal ────────────────────────────────────────────────
        _progress_hdr(_cyan("[%s] security   " % time.strftime("%H:%M:%S")) +
                      "trav-url  [%3d] " % len(TRAVERSAL_URL))
        for vec in TRAVERSAL_URL:
            payload = ("GET %s HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
                       % vec).encode("latin-1", "replace")
            _, raw = _probe(host, port, payload)
            stats["trav_url"] += 1
            leaked, marker = has_leak(raw)
            if leaked:
                _dot("!", "31")
                findings.append(("PATH_TRAVERSAL_URL",
                                  "vec=%r  marker=%r" % (vec, marker)))
            else:
                _dot(".")
        _progress_end()

        # ── X-File header traversal ───────────────────────────────────────────
        _progress_hdr(_cyan("[%s] security   " % time.strftime("%H:%M:%S")) +
                      "trav-hdr  [%3d] " % len(TRAVERSAL_HDR))
        for vec in TRAVERSAL_HDR:
            _, raw = _probe(host, port, _build("GET", "/download", headers={"X-File": vec}))
            stats["trav_hdr"] += 1
            leaked, marker = has_leak(raw)
            if leaked:
                _dot("!", "31")
                findings.append(("PATH_TRAVERSAL_HDR",
                                  "vec=%r  marker=%r" % (vec, marker)))
            else:
                _dot(".")
        _progress_end()

        # ── CRLF / response splitting ─────────────────────────────────────────
        _progress_hdr(_cyan("[%s] security   " % time.strftime("%H:%M:%S")) +
                      "crlf      [%3d] " % len(CRLF_VALUES))
        for val in CRLF_VALUES:
            payload = (b"GET /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX-Echo: "
                       + val + b"\r\n\r\n")
            _, raw = _probe(host, port, payload)
            stats["crlf"] += 1
            _, hdrs, _ = parse_hdrs(raw)
            if b"x-injected" in hdrs:
                _dot("!", "31")
                findings.append(("RESPONSE_SPLIT",
                                  "X-Echo %r caused X-Injected as parsed header name" % val))
            else:
                _dot(".")
        _progress_end()

        # ── Content-Type duplication (real \r\n injection) ────────────────────
        _progress_hdr(_cyan("[%s] security   " % time.strftime("%H:%M:%S")) +
                      "crlf-ct   [  2] ")
        for val in (b"ok\r\nContent-Type: text/html",
                    b"ok\r\nSet-Cookie: session=evil; Path=/"):
            payload = (b"GET /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX-Echo: "
                       + val + b"\r\n\r\n")
            _, raw = _probe(host, port, payload)
            _, hdrs, _ = parse_hdrs(raw)
            if len(hdrs.get(b"content-type", [])) > 1:
                _dot("!", "31")
                findings.append(("RESPONSE_SPLIT",
                                  "Content-Type duplicated by %r" % val))
            else:
                _dot(".")
        _progress_end()

        # ── Double-CRLF fake HTTP response ────────────────────────────────────
        _log("security", "double-CRLF body injection …")
        dbl = b"safe\r\n\r\nHTTP/1.1 200 Injected\r\nX-Injected: pwned\r\n\r\nbody"
        payload = (b"GET /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX-Echo: "
                   + dbl + b"\r\n\r\n")
        _, raw = _probe(host, port, payload)
        sep = raw.find(b"\r\n\r\n")
        if sep >= 0 and raw[sep+4:].startswith(b"HTTP/1.1 200 Injected"):
            findings.append(("RESPONSE_SPLIT",
                              "double CRLF created parseable second HTTP response"))
            _log("security", _red("  !! double-CRLF body injection confirmed"))

        # ── Information leakage on all routes ────────────────────────────────
        # Any route returning sensitive content = critical
        _log("security", "info leakage check across all routes …")
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
                _log("security", _red("  !! LEAK on %s: %r" % (path, marker)))

        # ── Contract violations: must return 500, never crash ─────────────────
        _log("security", "contract violations (%d routes × 10 hits) …" % len(VIOLATE_ROUTES))
        for route in VIOLATE_ROUTES:
            _progress_hdr(_cyan("[%s] security   " % time.strftime("%H:%M:%S")) +
                          "%-32s " % route)
            for _ in range(10):
                st, _ = _probe(host, port, _build("GET", route))
                if st == 500:
                    stats["violate_ok"] += 1; _dot(".")
                else:
                    stats["violate_bad"] += 1; _dot("!", "31")
                    findings.append(("VIOLATE_BAD_STATUS",
                                      "%s returned %s, expected 500" % (route, st)))
            _progress_end()

        # ── Server still alive ────────────────────────────────────────────────
        if not health(host, port, timeout=4.0):
            findings.append(("SERVER_DEAD", "unreachable after security phase"))

    finally:
        stop_bg.set()

    nf = len(findings)
    _log("security", _green("all clear") if nf == 0 else _red("%d finding(s)" % nf))

    rc = 2 if any(f[0] in ("PATH_TRAVERSAL_URL", "PATH_TRAVERSAL_HDR",
                            "RESPONSE_SPLIT", "INFO_LEAK") for f in findings) \
           else (1 if findings else 0)
    return {"name": "security", "rc": rc, "findings": findings, "stats": stats}


# ─────────────────────────────────────────────────────────────────────────────
# Phase 2 — PROTOCOL
# Server must survive every single vector. No assertion on status codes —
# 400/500/501 are all fine. Crash = fail.
# ─────────────────────────────────────────────────────────────────────────────

def phase_protocol(cfg, srv):
    host, port = cfg.host, cfg.port
    findings = []
    sent = 0

    corpora = [
        (HEADER_ABUSE,    "header-abuse"),
        (MALFORMED,       "malformed   "),
        (METHOD_FORMS,    "methods     "),
        (SMUGGLING,       "smuggling   "),
        (BODYBOMB_PAYLOADS,"bodybomb    "),
    ]

    _log("protocol", "started  (%d corpora, %d total vectors, zero concurrent load)"
         % (len(corpora), sum(len(c) for c, _ in corpora)))

    for corpus, label in corpora:
        _progress_hdr(_cyan("[%s] protocol   " % time.strftime("%H:%M:%S")) +
                      "%s [%3d] " % (label, len(corpus)))
        for vec in corpus:
            sent += 1
            raw_send(host, port, vec, rtimeout=2.0)
            _dot(".")

        alive_now = health(host, port, timeout=5.0)
        _progress_end(_green("alive") if alive_now else _red("DEAD"))

        if not alive_now:
            findings.append(("SERVER_DEAD_AFTER_%s" % label.strip().upper(),
                              "server unreachable after %s" % label.strip()))
        else:
            time.sleep(0.5)

    _log("protocol", "%d vectors — %s" % (sent,
         _green("survived all") if not findings
         else _red("%d corpus killed server" % len(findings))))

    return {"name": "protocol", "rc": 1 if findings else 0,
            "findings": findings, "stats": {"sent": sent}}


# ─────────────────────────────────────────────────────────────────────────────
# Phase 3 — FEATURES
# Every route verified for correct status, body content, and headers.
# Additional invariant checks beyond simple status.
# ─────────────────────────────────────────────────────────────────────────────

def phase_features(cfg, srv):
    host, port = cfg.host, cfg.port
    findings = []

    # Concurrent background load while checking
    stop_bg = threading.Event()
    def _bg():
        while not stop_bg.is_set():
            req(host, port, "GET", random.choice(LIGHT_PATHS), rtimeout=2.0)
    bg = [threading.Thread(target=_bg, daemon=True) for _ in range(8)]
    for t in bg: t.start()

    _log("features", "started  (8 background threads, %d route checks)" % len(ROUTE_CHECKS))

    try:
        # ── All routes concurrently ───────────────────────────────────────────
        p, f, details = _verify_all_routes(host, port)
        for d in details:
            findings.append(("ROUTE_FAIL", d))
        _log("features", "%d/%d routes correct%s" % (p, p+f,
             (" — " + _red("%d failed" % f)) if f else ""))

        # ── Additional invariants ─────────────────────────────────────────────

        # mw/injected: middleware must inject X-Route-MW: hit
        _progress_hdr(_cyan("[%s] features   " % time.strftime("%H:%M:%S")) + "mw:header          ")
        for _ in range(5):
            st, raw = req(host, port, "GET", "/mw/injected", rtimeout=3.0)
            _, hdrs, _ = parse_hdrs(raw)
            vals = hdrs.get(b"x-route-mw", [])
            if st != 200 or not vals or vals[0] != b"hit":
                _dot("!", "31")
                findings.append(("ROUTE_FAIL",
                                  "/mw/injected: X-Route-MW wrong (status=%s vals=%r)" % (st, vals)))
            else:
                _dot(".")
        _progress_end()

        # mw/skipnext: skipped MW header must NOT appear
        _progress_hdr(_cyan("[%s] features   " % time.strftime("%H:%M:%S")) + "mw:skipnext        ")
        for _ in range(5):
            st, raw = req(host, port, "GET", "/mw/skipnext", rtimeout=3.0)
            _, hdrs, _ = parse_hdrs(raw)
            if b"x-should-not-appear" in hdrs:
                _dot("!", "31")
                findings.append(("ROUTE_FAIL", "/mw/skipnext: skipped middleware header appeared"))
            else:
                _dot(".")
        _progress_end()

        # template/cond: branch isolation — wrong branches must not appear
        _progress_hdr(_cyan("[%s] features   " % time.strftime("%H:%M:%S")) + "tmpl:cond-isolation")
        for n, expected, forbidden in [
            (2, b"high",   [b"medium", b"low"]),
            (1, b"medium", [b"high",   b"low"]),
            (0, b"low",    [b"high",   b"medium"]),
        ]:
            st, raw = req(host, port, "GET", "/template/cond/%d" % n, rtimeout=3.0)
            b = body_of(raw)
            if st != 200 or expected not in b:
                _dot("!", "31")
                findings.append(("ROUTE_FAIL",
                                  "/template/cond/%d: %r missing" % (n, expected)))
            elif any(x in b for x in forbidden):
                _dot("!", "31")
                findings.append(("ROUTE_FAIL",
                                  "/template/cond/%d: forbidden %r present" % (
                                      n, [x for x in forbidden if x in b])))
            else:
                _dot(".")
        _progress_end()

        # template/inherit: base title must NOT appear (child overrode it)
        _progress_hdr(_cyan("[%s] features   " % time.strftime("%H:%M:%S")) + "tmpl:inherit       ")
        st, raw = req(host, port, "GET", "/template/inherit", rtimeout=3.0)
        b = body_of(raw)
        if b"Base Title" in b:
            _dot("!", "31")
            findings.append(("ROUTE_FAIL",
                              "/template/inherit: base title leaked — child override failed"))
        else:
            _dot(".")
        _progress_end()

        # Templates must respond Content-Type: text/html
        _progress_hdr(_cyan("[%s] features   " % time.strftime("%H:%M:%S")) + "tmpl:content-type  ")
        for path in ["/template/static", "/template/dynamic", "/template/loop",
                     "/template/include", "/template/inherit"]:
            st, raw = req(host, port, "GET", path, rtimeout=3.0)
            _, hdrs, _ = parse_hdrs(raw)
            ct = b"".join(hdrs.get(b"content-type", [b""]))
            if st != 200 or b"text/html" not in ct:
                _dot("!", "31")
                findings.append(("ROUTE_FAIL",
                                  "%s: Content-Type=%r (expected text/html)" % (path, ct)))
            else:
                _dot(".")
        _progress_end()

        # /chain: all three headers must be present with exact values
        _progress_hdr(_cyan("[%s] features   " % time.strftime("%H:%M:%S")) + "chain:all-headers  ")
        for _ in range(3):
            st, raw = req(host, port, "GET", "/chain", rtimeout=3.0)
            _, hdrs, _ = parse_hdrs(raw)
            ok = (st == 200
                  and hdrs.get(b"x-chain-a", [b""])[0] == b"alpha"
                  and hdrs.get(b"x-chain-b", [b""])[0] == b"beta"
                  and hdrs.get(b"x-chain-c", [b""])[0] == b"gamma")
            if not ok:
                _dot("!", "31")
                findings.append(("ROUTE_FAIL",
                                  "/chain: headers wrong (status=%s a=%r b=%r c=%r)" % (
                                      st,
                                      hdrs.get(b"x-chain-a"),
                                      hdrs.get(b"x-chain-b"),
                                      hdrs.get(b"x-chain-c"))))
            else:
                _dot(".")
        _progress_end()

        # /async/sleep: 20 concurrent requests — tests timer pool
        _log("features", "async/sleep × 20 concurrent …")
        ok20 = fail20 = 0
        lock20 = threading.Lock()
        def _async_req():
            nonlocal ok20, fail20
            st, raw = req(host, port, "GET", "/async/sleep", rtimeout=5.0)
            with lock20:
                if st == 200 and b"slept" in body_of(raw): ok20 += 1
                else:                                        fail20 += 1
        ts = [threading.Thread(target=_async_req) for _ in range(20)]
        for t in ts: t.start()
        for t in ts: t.join(timeout=10.0)
        if fail20 == 0:
            _log("features", _green("  async/sleep: 20/20 passed"))
        else:
            _log("features", _red("  async/sleep: %d/20 failed" % fail20))
            findings.append(("ROUTE_FAIL", "async/sleep concurrent: %d/20 failed" % fail20))

        if not health(host, port, timeout=4.0):
            findings.append(("SERVER_DEAD", "server unreachable after features phase"))

    finally:
        stop_bg.set()

    nf = len(findings)
    _log("features", _green("all clear") if nf == 0 else _red("%d failure(s)" % nf))
    return {"name": "features", "rc": 1 if findings else 0,
            "findings": findings, "stats": {}}


# ─────────────────────────────────────────────────────────────────────────────
# Phase 4 — CHAOS
# ─────────────────────────────────────────────────────────────────────────────

def phase_chaos(cfg, srv):
    host, port = cfg.host, cfg.port
    findings = []
    stats    = {}

    mpid = srv.pid()
    if not mpid:
        _log("chaos", _yellow("no master PID — skipped"))
        return {"name": "chaos", "rc": 0, "findings": [],
                "stats": {}, "notes": ["master PID unavailable"]}

    _log("chaos", "started  (master PID=%d)" % mpid)

    # 1. Single worker kills × 3
    _log("chaos", _bold("1/6 — single worker kill × 3"))
    for i in range(3):
        stop, ts = _light_flood(host, port, 8)
        _do_kill(mpid, stats, "kill-%d" % (i+1))
        ok = _chaos_recover_and_verify(host, port, findings, "kill-%d" % (i+1))
        stop.set()
        for t in ts: t.join(timeout=3.0)
        if not ok: break
        time.sleep(0.5)

    # 2. Kills under sustained load (30s, 1 kill/5s)
    _log("chaos", _bold("2/6 — kills under sustained load (30s)"))
    stop, ts = _light_flood(host, port, 16)
    t0 = time.time(); kn = 0
    while time.time() - t0 < 30.0:
        time.sleep(5.0)
        _, ok = _do_kill(mpid, stats, "load-%d" % (kn+1))
        if ok: kn += 1
    stop.set()
    for t in ts: t.join(timeout=5.0)
    _chaos_recover_and_verify(host, port, findings, "load-kills")
    _log("chaos", "  %d kills under load" % kn)

    # 3. Rapid-fire kills (1 kill/2s × 5)
    _log("chaos", _bold("3/6 — rapid-fire kills (1/2s × 5)"))
    stop, ts = _light_flood(host, port, 12)
    rk = 0
    for i in range(5):
        time.sleep(2.0)
        _, ok = _do_kill(mpid, stats, "rapid-%d" % (i+1))
        if ok: rk += 1
    stop.set()
    for t in ts: t.join(timeout=3.0)
    _chaos_recover_and_verify(host, port, findings, "rapid-fire", timeout=20.0)
    _log("chaos", "  %d rapid kills" % rk)

    # 4. Kill both workers simultaneously
    _log("chaos", _bold("4/6 — simultaneous dual-worker kill"))
    ws = children(mpid)
    if len(ws) >= 2:
        for w in ws[:2]:
            try:
                os.kill(w, signal.SIGKILL)
                stats["kills"] = stats.get("kills", 0) + 1
                _log("chaos", "  dual-kill PID %d" % w)
            except OSError as e:
                _log("chaos", _yellow("  dual-kill failed: %s" % e))
        _chaos_recover_and_verify(host, port, findings, "dual-kill", timeout=20.0)
    else:
        _log("chaos", _yellow("  only %d worker — skipping dual kill" % len(ws)))

    # 5. SIGSTOP → hammer 3s → SIGCONT
    _log("chaos", _bold("5/6 — SIGSTOP worker for 3s then SIGCONT"))
    ws = children(mpid)
    if ws:
        v = random.choice(ws)
        try:
            os.kill(v, signal.SIGSTOP)
            stats["stops"] = stats.get("stops", 0) + 1
            _log("chaos", "  SIGSTOP PID %d" % v)
            stop, ts = _light_flood(host, port, 8)
            time.sleep(3.0)
            stop.set()
            for t in ts: t.join(timeout=4.0)
            os.kill(v, signal.SIGCONT)
            _log("chaos", "  SIGCONT PID %d" % v)
            p, f, details = _verify_all_routes(host, port)
            if f > 0:
                findings.append(("SIGSTOP_FAIL",
                                  "%d routes wrong after SIGCONT: %s" % (f, details[:2])))
                _log("chaos", _red("  %d routes wrong after SIGCONT" % f))
            else:
                _log("chaos", _green("  all routes OK after SIGCONT"))
        except OSError as e:
            _log("chaos", _yellow("  SIGSTOP/SIGCONT failed: %s" % e))
    else:
        _log("chaos", _yellow("  no workers for SIGSTOP"))

    # 6. Kill under mixed load — idle connections + concurrent large responses.
    #
    # 75 half-open connections sit in read-wait on the server (no harness threads,
    # just held FDs). 12 threads each pull a 1MiB /big response in a loop.
    # 12 is enough to saturate the server's send path; more would just thrash
    # Python's scheduler on a 2-core CI runner without adding real server stress.
    # After 2s of active load a worker is killed. This exercises epoll cleanup of
    # in-progress sends AND idle-fd teardown in a single scenario.
    _log("chaos", _bold("6/6 — kill under mixed load (75 idle conns + 12 /big threads)"))

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
    _log("chaos", "  holding %d idle connections" % len(held))

    big_ok = big_fail = 0
    big_lock = threading.Lock()
    stop_big = threading.Event()
    def _big_req():
        nonlocal big_ok, big_fail
        while not stop_big.is_set():
            st, _ = req(host, port, "GET", "/big", rtimeout=8.0, ctimeout=2.0)
            with big_lock:
                if st == 200: big_ok += 1
                else:         big_fail += 1
    bts = [threading.Thread(target=_big_req, daemon=True) for _ in range(12)]
    for t in bts: t.start()
    time.sleep(2.0)

    _do_kill(mpid, stats, "mixed-load")

    stop_big.set()
    for t in bts: t.join(timeout=10.0)
    for s in held:
        try: s.close()
        except OSError: pass
    held.clear()

    _log("chaos", "  /big: ok=%d fail=%d  idle-conns=%d" % (big_ok, big_fail, stats["conn_held"]))
    _chaos_recover_and_verify(host, port, findings, "mixed-load")

    time.sleep(1.0)
    if not health(host, port, timeout=5.0):
        findings.append(("FINAL_DEAD", "server unreachable at end of chaos phase"))

    fm = _fetch_metrics(host, port)
    nf = len(findings)
    msg = "all scenarios survived" if nf == 0 else "%d issue(s)" % nf
    _log("chaos", "%s  (kills=%d  crashes=%s  restarts=%s)"
         % ((_green(msg) if nf == 0 else _red(msg)),
            stats.get("kills", 0),
            fm.get("process", {}).get("crashes", "?"),
            fm.get("process", {}).get("restarts", "?")))

    return {"name": "chaos", "rc": 1 if findings else 0,
            "findings": findings, "stats": stats, "final_metrics": fm}


# ─────────────────────────────────────────────────────────────────────────────
# ─────────────────────────────────────────────────────────────────────────────
# Report
# ─────────────────────────────────────────────────────────────────────────────

def print_report(results):
    print()
    _hdr("WFX TORTURE REPORT")
    overall = 0

    for r in results:
        name = r["name"].upper()
        rc   = r["rc"]
        overall = max(overall, rc)
        verdict = (_green("PASS") if rc == 0 else
                   _red("SECURITY FAILURE") if rc == 2 else _red("FAIL"))
        _sec(name, verdict)

        if name == "SECURITY":
            s = r["stats"]
            print("  traversal URL   : %d vectors" % s["trav_url"])
            print("  traversal X-File: %d vectors" % s["trav_hdr"])
            print("  CRLF injection  : %d vectors" % s["crlf"])
            print("  violate OK/BAD  : %d / %d" % (s["violate_ok"], s["violate_bad"]))
            print("  info leak checks: %d routes" % s["leak"])
            print()
            print("  CRLF: STRICT — only \\r\\n header-name splitting counts.")

        elif name == "PROTOCOL":
            print("  vectors sent: %d" % r["stats"]["sent"])
            print("  Corpora: header-abuse, malformed, methods, smuggling, bodybomb.")
            print("  Goal: server survives every vector. Any crash = FAIL.")

        elif name == "FEATURES":
            print("  Route checks: %d" % len(ROUTE_CHECKS))
            print("  Covers: segments (uint/int/string/uuid), groups,")
            print("  middleware (continue/break/skipnext), context, async,")
            print("  ImJson, RmJson, parse-json, chain-headers, metrics,")
            print("  templates (static/dynamic/cond/loop/include/inherit).")

        elif name == "CHAOS":
            s = r["stats"]
            print("  kills     : %d" % s.get("kills", 0))
            print("  stops     : %d" % s.get("stops", 0))
            print("  idle conns: %d (mixed-load scenario)" % s.get("conn_held", 0))
            fm = r.get("final_metrics", {})
            if fm:
                proc = fm.get("process", {})
                print("  crashes   : %s  restarts: %s"
                      % (proc.get("crashes"), proc.get("restarts")))
            print("  Route correctness verified after every recovery scenario.")

        for note in r.get("notes", []):
            print("  NOTE  %s" % note)

        if r.get("findings"):
            print("  %s" % _red("FINDINGS:"))
            seen = set()
            for label, detail in r["findings"]:
                key = (label, detail[:120])
                if key in seen: continue
                seen.add(key)
                print("    %s  %s" % (_red("[%s]" % label), detail))
                _gh_error("[%s] %s: %s" % (r["name"], label, detail[:200]))

    print()
    print("=" * 78)
    if overall == 0:   print(_bold(_green("  VERDICT: PASS — survived clean")))
    elif overall == 2: print(_bold(_red("  VERDICT: SECURITY FAILURE")))
    else:              print(_bold(_red("  VERDICT: FAIL")))
    print("=" * 78)
    print()
    return overall


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

PHASES = ["security", "protocol", "features", "chaos"]

PHASE_FNS = {
    "security": phase_security,
    "protocol": phase_protocol,
    "features": phase_features,
    "chaos":    phase_chaos,
}


def main():
    ap = argparse.ArgumentParser(
        description="WFX test harness.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
phases:
  security   path traversal (URL + X-File), CRLF/response-splitting,
             info leakage on all routes, contract violations
  protocol   malformed/abuse request vectors — server must survive, not crash
             corpora: header-abuse, malformed, methods, smuggling, bodybomb
  features   every route verified: exact status, body, headers, invariants
             branch isolation, header injection absence, Content-Type checks
  chaos      6 scenarios: worker kills x3, kills under sustained load,
             rapid-fire kills, dual-kill, SIGSTOP/SIGCONT,
             kill under mixed load (75 idle conns + 12 large-response threads)
             route correctness verified after every recovery

examples:
  python3 harness.py
  python3 harness.py --phase security
  python3 harness.py --list-phases
""")

    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--wfx", default="wfx", metavar="BIN")
    ap.add_argument("--app-dir", default="app", metavar="DIR")
    ap.add_argument("--phase", default="all", choices=["all"] + PHASES)
    ap.add_argument("--pid-file", default="~/.wfx/daemons/app.pid", metavar="PATH")
    ap.add_argument("--ready-timeout", type=int, default=20, metavar="S")
    ap.add_argument("--list-phases", action="store_true", help="list available phases and exit")
    ap.add_argument("--ci", action="store_true",
                    help="CI-friendly output: suppress inline dot progress, "
                         "emit GitHub Actions workflow commands (::group::, ::error::)")
    ap.add_argument("--phase-timeout", type=int, default=0, metavar="S",
                    help="max seconds per phase before marking TIMEOUT and continuing "
                         "(0 = unlimited; auto-set to 300 when --ci)")

    cfg = ap.parse_args()

    global _CI, _TTY
    if cfg.ci:
        _CI  = True
        _TTY = False  # force colors off; ::group:: etc. are only for GH Actions
        if cfg.phase_timeout == 0:
            cfg.phase_timeout = 300  # 5 min per phase in CI

    if cfg.list_phases:
        for p in PHASES: print(p)
        return 0

    srv = Server(cfg)
    try:
        srv.start()
        srv.wait_ready()
    except RuntimeError as e:
        print(_red("[FATAL]") + "  " + str(e), file=sys.stderr)
        return 1

    mpid = srv.pid()
    if mpid: _log("harness", "master PID=%d  (%s)" % (mpid, cfg.pid_file))
    else:    _log("harness", _yellow("warning: no master PID — chaos phase limited"))

    results = []; overall = 0
    phases_to_run = PHASES if cfg.phase == "all" else [cfg.phase]
    hb = _Heartbeat() if _CI else None

    try:
        for name in phases_to_run:
            print()
            if hb: hb.set_phase(name)
            _gh_group("phase: " + name)
            if cfg.phase_timeout > 0:
                r = _run_phase_timed(name, PHASE_FNS[name], cfg, srv, cfg.phase_timeout)
            else:
                r = PHASE_FNS[name](cfg, srv)
            _gh_endgroup()
            results.append(r)
            overall = max(overall, r["rc"])
            if r["rc"] == 2:
                _gh_error("security finding in phase %s — stopping" % name)
                _log("harness", _red("security finding — stopping"))
                break
            dead = [f for f in r.get("findings", [])
                    if "DEAD" in f[0] or "NO_RECOVERY" in f[0]]
            if dead and name != phases_to_run[-1]:
                _gh_warning("server appears dead after phase %s" % name)
                _log("harness", _yellow("server appears dead — skipping remaining phases"))
                break

    except KeyboardInterrupt:
        _log("harness", _yellow("interrupted"))

    finally:
        if hb: hb.stop()
        print()
        srv.stop()

    overall = max(overall, print_report(results))
    return overall


if __name__ == "__main__":
    sys.exit(main())