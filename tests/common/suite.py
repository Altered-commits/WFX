# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# The suite: configuration, lifecycle, and the run loop
#
# One shape for every audit. A suite declares its phases and the resources it needs, and this
# handles argument parsing, boot, phase dispatch, worker-death detection, teardown and the exit
# code. Phases never touch any of that

import argparse
import os
import threading
import time

from . import logs, term
from .report import Report, EXIT_NO_BOOT
from .server import Server

class Config:
    """Everything a suite is told from the command line, plus whatever it adds in configure()."""

    def __init__(self, args):
        self.host = args.host
        self.port = args.port
        self.wfx = args.wfx
        self.app_dir = args.app_dir
        self.ready_timeout = args.ready_timeout
        self.phase = args.phase
        self.wfx_logs = args.wfx_logs
        self.args = args  # suite-specific flags live here

class Context:
    """Passed to every phase. The single argument, so all phases share one signature."""

    def __init__(self, cfg, server, report):
        self.cfg = cfg
        self.server = server
        self.report = report
        self.resources = {}  # named resources from Suite.setup(), e.g. a mock upstream

    def phase(self, name):
        return self.report.phase(name)

    def __getattr__(self, name):
        # ctx.mock reads better than ctx.resources["mock"] in a phase body
        try:
            return self.__dict__["resources"][name]
        except KeyError:
            raise AttributeError(name)

class Heartbeat:
    """Prints "still running" every `interval` seconds, for CI only.

    A long phase that prints a dot per vector looks alive on a terminal and looks hung in a build-
    -log, where the dots are suppressed and some CI runners kill a job that prints nothing
    """

    def __init__(self, interval):
        self.interval = interval
        self._phase = "?"
        self._since = time.time()
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._run, daemon=True, name="heartbeat")

    def start(self):
        self._thread.start()

    def set_phase(self, name):
        with self._lock:
            self._phase, self._since = name, time.time()

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=5.0)

    def _run(self):
        while not self._stop.wait(self.interval):
            with self._lock:
                phase, elapsed = self._phase, time.time() - self._since

            term.log("runner", "still running: phase=%s elapsed=%.0fs" % (phase, elapsed))

class Suite:
    """Base class for an audit. Subclasses set `name`/`phases` and override the hooks they need."""

    name = "audit"
    description = "WFX audit"
    phases = {}  # ordered {phase name: callable(ctx)}

    # Stop the run on the first security finding. For suites where later phases would be measuring
    # an already-compromised server, so their results would be noise
    stop_on_security = False

    # Per-phase wall-clock budget in seconds, 0 for none. A phase that hangs is recorded as a
    # TIMEOUT finding and the run continues, rather than the whole audit stalling
    phase_timeout = 0

    # Confirm the master process actually exited during teardown, for suites that kill workers
    confirm_exit = False

    # Seconds between "still running" lines in CI, 0 for none. Silent on a terminal, where the
    # per-vector progress marks already show movement
    heartbeat = 0

    # Hooks, all optional
    def add_arguments(self, parser):
        """Suite-specific command-line flags."""

    def configure(self, cfg):
        """Derive extra config after parsing, before anything starts."""

    def build_server(self, cfg):
        return Server(cfg)

    def setup(self, ctx):
        """Start mocks and other resources. Register them in ctx.resources."""

    def before_phases(self, ctx):
        """Runs once the server is up, before the first phase.

        For evidence that must be snapshotted at boot, or preconditions worth failing fast on.
        Return False to skip the phases
        record why with ctx.phase(...).failed() first.
        """

    def teardown(self, ctx):
        """Stop whatever setup() started. Always runs, even after a failed boot."""

    # Runner
    def main(self, argv=None):
        parser = argparse.ArgumentParser(prog=self.name, description=self.description)
        self._add_common_arguments(parser)
        self.add_arguments(parser)
        args = parser.parse_args(argv)

        if args.ci:
            term.enable_ci()

        if args.list_phases:
            for name in self.phases:
                print(name)
            return 0

        cfg = Config(args)
        cfg.app_dir = os.path.abspath(cfg.app_dir)
        self.configure(cfg)

        report = Report()
        server = self.build_server(cfg)
        follower = logs.LogFollower(cfg.app_dir, mode=cfg.wfx_logs)
        ctx = Context(cfg, server, report)

        try:
            self.setup(ctx)
            server.start()

            # Follower first, so a crash during boot still prints
            follower.start()
            if server.wait_ready() is None:
                term.log("runner", term.red("WFX never answered %s within %ds"
                                             % (server.label, cfg.ready_timeout)))
                return report.render(booted=False, alive=False)

            if self.before_phases(ctx) is not False:
                self._run_phases(ctx)

            return report.render(alive=server.alive())
        finally:
            follower.stop()
            server.stop(confirm_exit=self.confirm_exit)
            self.teardown(ctx)

    def _run_phases(self, ctx):
        selected = list(self.phases) if ctx.cfg.phase == "all" else [ctx.cfg.phase]

        beat = Heartbeat(self.heartbeat) if self.heartbeat and term.is_ci() else None
        if beat:
            beat.start()

        try:
            self._dispatch(ctx, selected, beat)
        finally:
            if beat:
                beat.stop()

    def _dispatch(self, ctx, selected, beat):
        for name in selected:
            if beat:
                beat.set_phase(name)

            term.header("PHASE: " + name)
            term.group("phase: " + name)

            self._invoke(ctx, name, self.phases[name])

            term.endgroup()

            if self.stop_on_security and ctx.report.has_security_finding:
                term.error("security finding in phase %s, stopping" % name)
                term.log("runner", term.red("security finding, stopping the run"))
                return

            # A wall of empty results usually means the worker died mid-phase rather than the
            # assertions failing, so say so plainly and give the master time to revive it
            if not ctx.server.alive():
                term.log("runner", term.red("worker not responding after '%s', waiting for revival, "
                                             "see WFX logs above" % name))
                if ctx.server.await_revival() is None:
                    term.log("runner", term.red("worker did not come back within 15s"))

    def _invoke(self, ctx, name, fn):
        """Runs one phase. Neither an exception nor a hang costs the rest of the run."""
        failure = []

        def call():
            try:
                fn(ctx)
            except Exception as exc:
                failure.append(exc)

        if not self.phase_timeout:
            call()
        else:
            worker = threading.Thread(target=call, daemon=True, name="phase-" + name)
            worker.start()
            worker.join(self.phase_timeout)

            if worker.is_alive():
                # Left running: it holds the server, so the next phase may see the fallout
                term.log("runner", term.red("phase %s exceeded %ds" % (name, self.phase_timeout)))
                ctx.report.phase(name).failed("TIMEOUT", "exceeded %ds" % self.phase_timeout)
                return

        if failure:
            term.log("runner", term.red("phase %s crashed: %r" % (name, failure[0])))
            ctx.report.phase(name).failed("phase-exception", repr(failure[0]))

    def _add_common_arguments(self, parser):
        parser.add_argument("--host", default="127.0.0.1")
        parser.add_argument("--port", type=int, default=8080, help="WFX inbound port")
        parser.add_argument("--wfx", default="wfx", metavar="BIN")
        parser.add_argument("--app-dir", default="app", metavar="DIR")
        parser.add_argument("--ready-timeout", type=int, default=30, metavar="S")
        parser.add_argument("--phase", default="all", choices=["all"] + list(self.phases))
        parser.add_argument("--list-phases", action="store_true", help="list phases and exit")
        parser.add_argument("--wfx-logs", default=logs.IMPORTANT, choices=list(logs.MODES),
                            help="stream WFX logs live (crash=dumps and sanitizer reports only, "
                                 "important=those plus WRN/ERR/FTL, all=everything, off=none)")
        parser.add_argument("--ci", action="store_true",
                            help="no colors, GitHub Actions groups and error annotations")

def run(suite_cls):
    """Entry point: `if __name__ == "__main__": common.run(MySuite)`."""
    import sys

    try:
        sys.exit(suite_cls().main())
    except KeyboardInterrupt:
        sys.exit(130)
