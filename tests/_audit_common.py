#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Shared infrastructure for the WFX test harnesses (base_audit, endpoint_audit,
# tls_audit). Nothing in this file decides what a test checks. It only
# provides the boilerplate every harness needs: colored terminal output,
# GitHub Actions log grouping and error annotations under --ci, a minimal raw
# HTTP client, a live WFX log follower, and the check/report bookkeeping used
# by the bucket-style harnesses (endpoint_audit, tls_audit).
#
# base_audit uses a different phase-result shape (timed threads, findings
# lists) and does not use the Results/check/format_report helpers here; it
# still uses the color/log/gh_* helpers and the raw HTTP client.

import json
import os
import signal
import socket
import sys
import threading
import time

# CI mode
#
# Every harness exposes the same --ci flag and calls enable_ci_mode() once
# it's set. In CI mode: colors are disabled (ANSI codes don't help in the
# Actions log viewer), phases can be wrapped in ::group::/::endgroup:: so
# they collapse in the log, and failing checks get ::error:: annotations so
# they show up in the PR Checks UI instead of only in the raw log text.

CI = False
TTY = sys.stdout.isatty()

def enable_ci_mode():
    global CI, TTY
    CI = True
    TTY = False

# Colors
def _c(code, text):
    return ("\x1b[%sm%s\x1b[0m" % (code, text)) if TTY else text

# Public alias for a harness that needs a color _audit_common doesn't name
# directly (e.g. magenta in base_audit's chaos-phase output).
color = _c

def green(text):  return _c("32", text)
def red(text):    return _c("31", text)
def yellow(text): return _c("33", text)
def cyan(text):   return _c("36", text)
def bold(text):   return _c("1", text)

# GitHub Actions workflow commands (no-ops outside --ci)
def gh_group(name):
    if CI:
        print("::group::" + name, flush=True)

def gh_endgroup():
    if CI:
        print("::endgroup::", flush=True)

def gh_error(msg):
    if CI:
        print("::error::" + msg, flush=True)

def gh_warning(msg):
    if CI:
        print("::warning::" + msg, flush=True)

# Logging
def log(tag, msg="", c=None):
    print("%s %s" % ((c or cyan)("[%s]" % tag), msg), flush=True)

def hdr(title, width=78):
    print("\n" + bold("═" * width))
    print(bold("  " + title))
    print(bold("═" * width), flush=True)

# Raw HTTP client (stdlib socket only, never raises)
def raw_send(host, port, payload, rtimeout=8.0, ctimeout=5.0, rmax=8 << 20):
    """Send raw bytes over a plain TCP socket, read until close or timeout."""
    try:
        s = socket.create_connection((host, port), timeout=ctimeout)
    except OSError:
        return None
    try:
        s.sendall(payload)
        s.settimeout(rtimeout)
        chunks, total = [], 0
        while total < rmax:
            try:
                d = s.recv(65536)
            except (socket.timeout, OSError):
                break
            if not d:
                break
            chunks.append(d)
            total += len(d)
        return b"".join(chunks)
    except OSError:
        return None
    finally:
        try:
            s.close()
        except OSError:
            pass

def build_request(method, path, headers=None, body=b""):
    if isinstance(body, str):
        body = body.encode("latin-1")
    lines = ["%s %s HTTP/1.1" % (method, path), "Host: h", "Connection: close"]
    if headers:
        for k, v in headers.items():
            lines.append("%s: %s" % (k, v))
    if body or method in ("POST", "PUT", "PATCH"):
        lines.append("Content-Length: %d" % len(body))
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body

def response_body(raw):
    i = raw.find(b"\r\n\r\n")
    return raw[i + 4:] if i >= 0 else b""

def response_status(raw):
    if not raw or not raw.startswith(b"HTTP/"):
        return None
    try:
        return int(raw.split(b" ", 2)[1])
    except Exception:
        return None

# Live WFX log follower
#
# The worker runs detached, so its logs only exist on disk. When a worker
# crashes, the master revives it fast and the new worker truncates
# worker-N.log, so a read done after the fact misses the crash. This thread
# tails the log files continuously (like `tail -F`), detects truncation
# (worker revived) or rotation (inode changed), and re-follows the fresh file
# without losing what it already printed. Crash dumps are streamed in full;
# worker/master logs are filtered to WRN/ERR/FTL and crash-related keywords
# by default to stay readable.
_IMPORTANT_TAGS = ("[WRN]", "[ERR]", "[FTL]", "[CRT]")
_IMPORTANT_KEYWORDS = ("died", "crash", "abort", "signal", "segfault", "segv", "sigsegv",
                      "terminate", "revive", "revival", "panic", "assert", "fatal")

def _is_important(line):
    if any(tag in line for tag in _IMPORTANT_TAGS):
        return True
    low = line.lower()
    return any(kw in low for kw in _IMPORTANT_KEYWORDS)

class LogFollower(threading.Thread):
    def __init__(self, app_dir, mode="important", interval=0.1):
        super().__init__(daemon=True)
        self.app_dir = app_dir
        self.mode = mode          # "off" | "important" | "all"
        self.interval = interval
        self._stop = threading.Event()
        self._pos = {}            # path -> (inode, offset)

    def _dirs(self):
        root = os.path.join(self.app_dir, "logs")
        return [(os.path.join(root, "default_logs"), False),
               (os.path.join(root, "crash_logs"), True)]

    def _emit(self, name, line, is_crash):
        if is_crash:
            print("%s %s" % (red("[wfx-crash:%s]" % name), line), flush=True)
        elif self.mode == "all" or _is_important(line):
            print("%s %s" % (cyan("[wfx:%s]" % name), line), flush=True)

    def _scan_once(self):
        for d, is_crash in self._dirs():
            if not os.path.isdir(d):
                continue
            for name in sorted(os.listdir(d)):
                path = os.path.join(d, name)
                try:
                    st = os.stat(path)
                except OSError:
                    continue
                if not os.path.isfile(path):
                    continue

                ino, off = self._pos.get(path, (None, 0))
                if ino is None:
                    ino, off = st.st_ino, 0
                elif st.st_ino != ino or st.st_size < off:
                    if not is_crash:
                        print(yellow("[wfx] %s truncated (worker revived?), re-following" % name), flush=True)
                    ino, off = st.st_ino, 0

                if st.st_size > off:
                    try:
                        with open(path, "r", errors="replace") as f:
                            f.seek(off)
                            chunk = f.read()
                            off = f.tell()
                    except OSError:
                        continue
                    for line in chunk.splitlines():
                        if line.strip():
                            self._emit(name, line, is_crash)
                self._pos[path] = (ino, off)

    def run(self):
        if self.mode == "off":
            return
        while not self._stop.is_set():
            try:
                self._scan_once()
            except Exception:
                pass
            self._stop.wait(self.interval)
        try:
            self._scan_once()   # final drain
        except Exception:
            pass

    def stop(self):
        self._stop.set()

# Check/report bookkeeping
#
# A harness collects checks into named phase buckets, then prints one table
# at the end. Each check is (name, passed, security, detail); security marks
# a failure as a security finding rather than a plain correctness failure,
# which the caller's exit-code logic treats as more severe.
class Results:
    def __init__(self):
        self.phases = []   # [(phase_name, [(name, passed, security, detail), ...])]

    def phase(self, name):
        bucket = []
        self.phases.append((name, bucket))
        return bucket

def check(bucket, name, passed, detail="", security=False):
    bucket.append((name, bool(passed), security, detail))
    mark = green("ok  ") if passed else (red("SEC ") if security else red("FAIL"))
    print("  %s %-48s %s" % (mark, name, "" if passed else yellow(detail)), flush=True)
    if not passed:
        gh_error(("%s: %s" % (name, detail)) if detail else name)
    return passed

def format_report(results):
    """Prints the phase/check table. Returns (passed, total, sec_fail, fail)."""
    total = passed = sec_fail = fail = 0
    for phase, bucket in results.phases:
        p = sum(1 for _, ok, _, _ in bucket if ok)
        n = len(bucket)
        total += n
        passed += p
        color = green if p == n else red
        print("  %-14s %s" % (phase, color("%d/%d" % (p, n))))
        for name, ok, sec, detail in bucket:
            if not ok:
                if sec:
                    sec_fail += 1
                else:
                    fail += 1
                tag = red("SECURITY") if sec else red("fail")
                print("      %s  %s  %s" % (tag, name, yellow(detail)))
    return passed, total, sec_fail, fail

# Shared argparse scaffolding 
def add_common_args(ap, phases):
    """Adds the flags every harness needs. Callers add their own on top."""
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080, help="WFX inbound port")
    ap.add_argument("--wfx", default="wfx", metavar="BIN")
    ap.add_argument("--app-dir", default="app", metavar="DIR")
    ap.add_argument("--ready-timeout", type=int, default=30, metavar="S")
    ap.add_argument("--phase", default="all", choices=["all"] + list(phases))
    ap.add_argument("--list-phases", action="store_true", help="list available phases and exit")
    ap.add_argument("--wfx-logs", default="important", choices=["off", "important", "all"],
                    help="stream WFX worker/master logs into this terminal live "
                         "(important=WRN/ERR/FTL+crash keywords, all=everything, off=none)")
    ap.add_argument("--ci", action="store_true",
                    help="CI-friendly output: no colors, GitHub Actions log groups "
                         "and error annotations for failing checks")