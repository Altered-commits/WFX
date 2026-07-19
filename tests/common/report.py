# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Results collection, the end-of-run table, and the exit code every suite shares

from . import term

# Exit codes, identical across all suites so CI can key on them
EXIT_OK = 0
EXIT_FAIL = 1     # a non-security check failed, or the worker was dead at the end
EXIT_SECURITY = 2 # a vector whose failure is a vulnerability
EXIT_NO_BOOT = 3  # WFX never reached /health, so nothing was exercised

class Check:
    __slots__ = ("name", "passed", "security", "detail")

    def __init__(self, name, passed, security, detail):
        self.name = name
        self.passed = bool(passed)
        self.security = security
        self.detail = detail

class Phase:
    """One named group of checks. Records as it goes so failures print live, not at the end."""

    def __init__(self, name):
        self.name = name
        self.checks = []

    def check(self, name, passed, detail="", security=False):
        entry = Check(name, passed, security, detail)
        self.checks.append(entry)

        mark = term.green("ok  ") if entry.passed else term.red("SEC " if security else "FAIL")
        line = "  %s %-48s %s" % (mark, name, "" if entry.passed else term.yellow(detail))
        print(line.rstrip(), flush=True)

        if not entry.passed:
            term.error(("%s: %s" % (name, detail)) if detail else name)

        return entry.passed

    # Convenience wrappers, so intent reads off the call site
    def secure(self, name, passed, detail=""):
        return self.check(name, passed, detail, security=True)

    def failed(self, name, detail=""):
        return self.check(name, False, detail)

    def record(self, findings, security=(), vectors=None):
        """Bulk-records (label, detail) pairs from a phase that collects failures as it goes.

        Suites that drive hostile load tend to accumulate findings rather than assert per vector,
        since the interesting output is "what went wrong", not a pass count. `security` is the set
        of labels whose failure is a vulnerability, `vectors` how many were driven.
        """
        for label, detail in findings:
            self.check(label, False, detail, security=label in security)

        # A clean phase would otherwise record nothing at all and render as 0/0, which reads
        # exactly like a phase that never ran
        if not findings:
            self.check("no findings%s" % (" (%d vectors)" % vectors if vectors else ""), True)

    @property
    def passed(self):
        return sum(1 for c in self.checks if c.passed)

    @property
    def total(self):
        return len(self.checks)

class Report:
    def __init__(self):
        self.phases = []

    def phase(self, name):
        """The phase by that name, created on first use.

        Returning the existing one matters: the runner records a crashed phase against its own
        name, and a fresh object there would report the same phase twice, once with the checks
        that did run and once with the failure.
        """
        for entry in self.phases:
            if entry.name == name:
                return entry

        entry = Phase(name)
        self.phases.append(entry)
        return entry

    @property
    def has_security_finding(self):
        return any(not c.passed and c.security for phase in self.phases for c in phase.checks)

    def tally(self):
        passed = total = security = failed = 0

        for phase in self.phases:
            passed += phase.passed
            total += phase.total
            for c in phase.checks:
                if c.passed:
                    continue
                if c.security:
                    security += 1
                else:
                    failed += 1

        return passed, total, security, failed

    def render(self, booted=True, alive=True):
        """Prints the summary table and returns the process exit code."""
        term.header("REPORT")
        passed, total, security, failed = self.tally()

        for phase in self.phases:
            # Zero checks is not success: the phase either recorded nothing or never ran
            if phase.total == 0:
                print("  %-16s %s" % (phase.name, term.red("no checks recorded")))
                continue

            color = term.green if phase.passed == phase.total else term.red
            print("  %-16s %s" % (phase.name, color("%d/%d" % (phase.passed, phase.total))))

            for c in phase.checks:
                if not c.passed:
                    tag = term.red("SECURITY") if c.security else term.red("fail")
                    print("      %s  %s  %s" % (tag, c.name, term.yellow(c.detail)))

        print()
        if not booted:
            term.log("runner", term.red("WFX never reached /health, boot crash, see [wfx-crash:*] above"))

        term.log("runner", "server alive at end: %s" % (term.green("yes") if alive else term.red("NO")))
        print(term.bold("  TOTAL  %s   security: %s   other: %s" % (
            term.green("%d/%d passed" % (passed, total)),
            term.red(str(security)) if security else term.green("0"),
            term.red(str(failed)) if failed else term.green("0"))))

        if not booted:
            return EXIT_NO_BOOT
        if security:
            return EXIT_SECURITY
        if failed or not alive:
            return EXIT_FAIL

        # Nothing was actually asserted, so there is nothing to call a pass
        if total == 0:
            term.log("runner", term.red("no checks were recorded, treating as a failure"))
            return EXIT_FAIL

        return EXIT_OK
