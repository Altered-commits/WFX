# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Shared setup and plumbing for the WFX audits. See tests/README.md for how to write one
#
# Nothing here decides what a test checks; it is the plumbing every audit needs, so that a suite
# file contains only vectors and assertions
#
#   term     colors, headers, CI annotations
#   net      raw HTTP over TCP or TLS, deliberately not a real client
#   logs     tailing WFX's log files, sanitizer wiring
#   server   the WFX process under test
#   report   results, the summary table, exit codes
#   suite    configuration, lifecycle, the run loop

from . import logs, net, server, term
from .logs import LogFollower, launch_env
from .report import EXIT_FAIL, EXIT_NO_BOOT, EXIT_OK, EXIT_SECURITY, Report
from .server import Server, await_health, health, http_probe, pid_alive, poll, tls_probe
from .suite import Config, Context, Suite, run

__all__ = [
    "Config", "Context", "LogFollower", "Report", "Server", "Suite",
    "EXIT_OK", "EXIT_FAIL", "EXIT_SECURITY", "EXIT_NO_BOOT",
    "http_probe", "tls_probe", "health", "await_health", "poll", "pid_alive", "launch_env",
    "run",
    "logs", "net", "server", "term",
]
