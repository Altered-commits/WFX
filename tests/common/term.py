# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Terminal output: colors, section headers, and the GitHub Actions workflow commands
#
# Everything here is a no-op-or-plain-text when stdout is not a TTY or --ci is set, so a suite
# never has to ask where it is running

import sys
import time

_ci = False
_tty = sys.stdout.isatty()

def enable_ci():
    """Colors off, workflow commands on. Called once, from argument parsing."""
    global _ci, _tty
    _ci, _tty = True, False

def is_ci():
    return _ci

# Colors
def paint(code, text):
    return ("\x1b[%sm%s\x1b[0m" % (code, text)) if _tty else text

def green(text):
    return paint("32", text)
def red(text):
    return paint("31", text)
def yellow(text):
    return paint("33", text)
def cyan(text):
    return paint("36", text)
def magenta(text):
    return paint("35", text)
def bold(text):
    return paint("1", text)

# Structured output
def log(tag, msg="", color=cyan):
    print("[%s] %s %s" % (time.strftime("%H:%M:%S"), color("%-10s" % tag), msg), flush=True)

def header(title, width=78):
    print("\n" + bold("=" * width))
    print(bold("  " + title))
    print(bold("=" * width), flush=True)

def rule(width=78):
    print("=" * width)

class Progress:
    """A one-line ticker: one mark per vector instead of one line per vector.

    A phase driving hundreds of hostile requests would otherwise either print nothing for a minute
    or bury its result in output, and neither shows whether it is still moving. Under --ci the marks
    are dropped, since a build log has no cursor to overwrite.
    """

    def __init__(self, tag, label, count=None):
        text = cyan("[%s] %-10s " % (time.strftime("%H:%M:%S"), tag)) + "%-19s" % label
        if count is not None:
            text += " [%3d]" % count

        self._done = False
        if _ci:
            print(text, flush=True)
        else:
            sys.stdout.write(text + " ")
            sys.stdout.flush()

    def mark(self, char=".", color=green):
        if not _ci:
            sys.stdout.write(color(char))
            sys.stdout.flush()

    def ok(self):
        self.mark()

    def bad(self, char="!"):
        self.mark(char, red)

    def finish(self, suffix=""):
        if self._done:
            return
        self._done = True

        if not _ci:
            print((" " + suffix) if suffix else "", flush=True)
        elif suffix:
            print("  " + suffix, flush=True)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.finish()
        return False

def progress(tag, label, count=None):
    """`with term.progress("security", "trav-url", len(vectors)) as pr:` then pr.ok()/pr.bad()."""
    return Progress(tag, label, count)

# GitHub Actions workflow commands, silent outside CI
def group(name):
    if _ci:
        print("::group::" + name, flush=True)

def endgroup():
    if _ci:
        print("::endgroup::", flush=True)

def error(msg):
    if _ci:
        print("::error::" + msg, flush=True)

def warning(msg):
    if _ci:
        print("::warning::" + msg, flush=True)
