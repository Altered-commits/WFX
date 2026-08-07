# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# The WFX server under test
#
# Every suite drives the same daemon the same way: `wfx run <app> --detach`, poll /health until it
# answers, then `wfx control stop`. They differ only in the health probe (tls_audit speaks TLS to
# WFX), a couple of flags, and the working directory, so those are constructor arguments

import os
import signal
import subprocess
import time

from . import net, term
from .logs import launch_env

def health(host, port, timeout=2.0, tls=False):
    """Is WFX answering? The one definition of alive, for probes and phases alike."""
    raw = net.send(host, port, net.request("GET", "/health"),
                   rtimeout=timeout, ctimeout=timeout, tls=tls)
    return net.status(raw) == 200

def poll(predicate, timeout, interval=0.3):
    """Seconds taken for predicate to hold, or None if it never did within timeout."""
    start = time.time()
    while time.time() - start < timeout:
        if predicate():
            return time.time() - start
        time.sleep(interval)

    return None

def await_health(host, port, timeout, tls=False, probe_timeout=1.5):
    """Seconds until WFX answers /health, or None. For phases that only have a host and port."""
    return poll(lambda: health(host, port, probe_timeout, tls), timeout)

def http_probe(cfg, timeout):
    return health(cfg.host, cfg.port, timeout)

def tls_probe(cfg, timeout):
    return health(cfg.host, cfg.port, timeout, tls=True)

def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False

class Server:
    def __init__(self, cfg, flags=(), cwd=None, app=None, probe=http_probe, label="/health"):
        self.cfg = cfg
        self.flags = list(flags)
        # `wfx run` given an absolute app path from another cwd builds and logs "Server running",-
        # -then the detached daemon fails to bind, so the app is always named relative to its parent
        self.cwd = cwd or os.path.dirname(cfg.app_dir.rstrip("/"))
        self.app = app or os.path.basename(cfg.app_dir.rstrip("/"))
        self.probe = probe
        self.label = label
        self._started = False
        self._pid = None

    def start(self):
        cmd = [self.cfg.wfx, "run", self.app, "--port", str(self.cfg.port), *self.flags, "--detach"]
        term.log("server", "starting%s: %s" % ((" (cwd=%s)" % self.cwd) if self.cwd else "", " ".join(cmd)))

        result = subprocess.run(cmd, cwd=self.cwd, capture_output=True, text=True,
                                env=launch_env(self.cfg.app_dir))
        if result.returncode != 0:
            raise RuntimeError("wfx run failed (rc=%d): %s %s"
                               % (result.returncode, result.stdout.strip(), result.stderr.strip()))

        self._started = True
        term.log("server", term.green("detached OK"))

    def wait_ready(self):
        """Seconds taken to answer, or None if it never did within ready_timeout."""
        term.log("server", "waiting for %s ..." % self.label)

        took = self._await(self.cfg.ready_timeout)
        if took is not None:
            term.log("server", term.green("up in %.1fs" % took))

        return took

    def alive(self):
        return self.probe(self.cfg, 2.5)

    def await_revival(self, timeout=15.0):
        """Waits out a worker restart. Seconds taken to come back, or None."""
        return self._await(timeout)

    def _await(self, timeout):
        return poll(lambda: self.probe(self.cfg, 1.5), timeout)

    def pid(self):
        """Master PID from the daemon registry, for suites that signal workers directly.

        The file is `key=value` lines written by DaemonRegistry::Write (utils/daemon/
        daemon_registry.cpp), named for the project, which is the argument given to
        `wfx run`. A bare integer is accepted too, in case that format ever changes back.
        """
        if self._pid:
            return self._pid

        pid_file = getattr(self.cfg, "pid_file", None) or \
                   os.path.join("~/.wfx/daemons", self.app + ".pid")

        try:
            text = open(os.path.expanduser(pid_file)).read()
        except OSError:
            return None

        for line in text.split("\n"):
            key, _, value = line.partition("=")
            if key.strip() == "pid":
                try:
                    self._pid = int(value.strip()) or None
                except ValueError:
                    self._pid = None
                return self._pid

        try:
            self._pid = int(text.strip()) or None
        except ValueError:
            self._pid = None

        return self._pid

    def stop(self, confirm_exit=False):
        if not self._started:
            return

        term.log("server", "stopping ...")
        subprocess.run([self.cfg.wfx, "control", "stop", self.app], cwd=self.cwd,
                       capture_output=True, text=True)

        if not confirm_exit:
            return

        # Suites that kill workers can't trust `control stop` alone, so confirm the master is gone
        master = self.pid()
        if not master:
            term.log("server", term.green("stopped"))
            return

        deadline = time.time() + 10.0
        while time.time() < deadline:
            if not pid_alive(master):
                term.log("server", term.green("stopped (PID %d exited)" % master))
                return
            time.sleep(0.25)

        term.log("server", term.yellow("PID %d still alive, sending SIGKILL" % master))
        try:
            os.kill(master, signal.SIGKILL)
        except OSError:
            pass
