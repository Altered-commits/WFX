# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# WFX writes its logs to files under <app>/logs, and `wfx run --detach` sends stdout/stderr to
# /dev/null, so the only way to see a worker die is to tail those files while the run happens
#
# Two directories, treated differently:
#   default_logs  ordinary INF/WRN/ERR logging, filtered by mode
#   crash_logs    crash dumps, always shown, and where sanitizer reports are pointed
#
# Sanitizers are the reason crash_logs matters here: ASan/UBSan only ever write to stderr, which
# detaching discards, so without launch_env() a worker dying of a memory bug is indistinguishable
# from a clean exit

import os
import threading

from . import term

# Reports land here so the follower picks them up like any other dump
_SANITIZER_PREFIX = "sanitizer"

# Modes, least to most verbose
OFF = "off"              # nothing
CRASH = "crash"          # dumps and sanitizer reports only
IMPORTANT = "important"  # those, plus WRN/ERR/FTL and crash-adjacent keywords
ALL = "all"              # every line
MODES = (OFF, CRASH, IMPORTANT, ALL)

_LEVEL_TAGS = ("[WRN]", "[ERR]", "[FTL]", "[CRT]")
_KEYWORDS = ("died", "crash", "abort", "signal", "segfault", "segv", "sigsegv",
             "terminate", "revive", "revival", "panic", "assert", "fatal")

def _is_important(line):
    if any(tag in line for tag in _LEVEL_TAGS):
        return True

    lowered = line.lower()
    return any(word in lowered for word in _KEYWORDS)

def launch_env(app_dir):
    """Environment for `wfx run`, plus a wiped crash_logs to launch it into.

    Old dumps are removed so anything the follower prints in red belongs to this run; a report from
    hours ago replaying at startup would otherwise read as a fresh crash. default_logs is left alone.
    """
    crash_dir = os.path.join(app_dir, "logs", "crash_logs")

    try:
        os.makedirs(crash_dir, exist_ok=True)
        for name in os.listdir(crash_dir):
            path = os.path.join(crash_dir, name)
            if os.path.isfile(path):
                os.remove(path)
    except OSError:
        return dict(os.environ)

    # One prefix for both: they share a runtime, so the last log_path set wins and separate names
    # would mislabel whichever report actually lands
    # halt_on_error=1 stops the worker at the FIRST error instead of limping on and possibly
    # papering over the crash with a revival, so the report on disk is the whole story and
    # scan_crash_reports() below can turn it into a hard failure
    env = dict(os.environ)
    log_path = "log_path=%s" % os.path.join(crash_dir, _SANITIZER_PREFIX)
    env["ASAN_OPTIONS"] = "%s:halt_on_error=1" % log_path
    env["UBSAN_OPTIONS"] = "%s:print_stacktrace=1:halt_on_error=1" % log_path

    return env

def scan_crash_reports(app_dir):
    """Sanitizer (ASan/UBSan) report files written during this run, as (path, first_line).

    launch_env() wipes crash_logs at boot and points ASAN/UBSAN log_path at the
    'sanitizer' prefix here, so any 'sanitizer*' file is a memory-safety error from
    THIS run. The log follower prints these in red but never counts them, so without
    this a UAF that kills-then-revives a worker scrolls by and the suite still exits
    green. The suite turns a non-empty result into a hard failure
    """
    crash_dir = os.path.join(app_dir, "logs", "crash_logs")
    out = []
    try:
        names = sorted(os.listdir(crash_dir))
    except OSError:
        return out

    for name in names:
        if not name.startswith(_SANITIZER_PREFIX):
            continue
        path = os.path.join(crash_dir, name)
        if not os.path.isfile(path):
            continue

        first = ""
        try:
            with open(path, "r", errors="replace") as handle:
                for line in handle:
                    stripped = line.strip()
                    if stripped:
                        first = stripped
                        break
        except OSError:
            pass
        out.append((path, first))

    return out

class LogFollower(threading.Thread):
    """Tails the app's log directories and prints new lines as they appear, like `tail -F`."""

    def __init__(self, app_dir, mode=IMPORTANT, interval=0.1):
        super().__init__(daemon=True)
        self.app_dir = app_dir
        self.mode = mode
        self.interval = interval
        self._stop = threading.Event()
        self._pos = {}        # path -> (inode, offset)
        self._primed = False  # first pass seeds existing files at EOF, see _scan

    def _dirs(self):
        root = os.path.join(self.app_dir, "logs")
        return ((os.path.join(root, "default_logs"), False),
                (os.path.join(root, "crash_logs"), True))

    def _emit(self, name, line, is_crash):
        if is_crash:
            print("%s %s" % (term.red("[wfx-crash:%s]" % name), line), flush=True)
        elif self.mode == ALL or (self.mode == IMPORTANT and _is_important(line)):
            print("%s %s" % (term.cyan("[wfx:%s]" % name), line), flush=True)

    def _scan(self):
        for directory, is_crash in self._dirs():
            if not os.path.isdir(directory):
                continue

            for name in sorted(os.listdir(directory)):
                path = os.path.join(directory, name)
                try:
                    st = os.stat(path)
                except OSError:
                    continue
                if not os.path.isfile(path):
                    continue

                inode, offset = self._pos.get(path, (None, 0))
                if inode is None:
                    # default_logs survives across runs, so the first pass starts at EOF and reports
                    # only what this run appends. Files appearing later are new, so read from 0
                    inode, offset = st.st_ino, (0 if self._primed else st.st_size)
                elif st.st_ino != inode or st.st_size < offset:
                    # Truncated or rotated: WFX does this on boot and on every worker revival, both
                    # of which master.log already reports, so re-follow silently
                    inode, offset = st.st_ino, 0

                if st.st_size > offset:
                    try:
                        with open(path, "r", errors="replace") as handle:
                            handle.seek(offset)
                            chunk = handle.read()
                            offset = handle.tell()
                    except OSError:
                        continue

                    for line in chunk.splitlines():
                        if line.strip():
                            self._emit(name, line, is_crash)

                self._pos[path] = (inode, offset)

    def run(self):
        if self.mode == OFF:
            return

        while not self._stop.is_set():
            try:
                self._scan()
            except Exception:
                pass

            self._primed = True
            self._stop.wait(self.interval)

        try:
            self._scan()  # final drain, so a crash during teardown still prints
        except Exception:
            pass

    def stop(self):
        self._stop.set()
