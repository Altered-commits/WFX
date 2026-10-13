# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Per-run edits to an app's wfx.local.toml
#
# The tomls are tracked, so nothing a suite points them at (an absolute cert path, a variant it
# restarts into) may outlive the run. The runner restores every patched file once the server has
# stopped, and a phase that needs the baseline back mid-run restores it itself

import os
import re

# path -> the text it held before this run first patched it
_originals = {}

def _render(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        return '"%s"' % value
    return "[%s]" % ", ".join(_render(item) for item in value)

def patch(path, key, value):
    """Sets key to value (a str, bool, int or list of them), keeping the line's comment.

    The key has to appear exactly once, so a typo or a renamed key fails loudly instead of the
    server quietly booting on its baseline.
    """
    path = os.path.abspath(path)
    with open(path, newline="") as handle:
        original = handle.read()

    pattern = r'(?m)^(\s*%s\s*=\s*)("[^"]*"|\[[^\]\n]*\]|[^\s#]+)' % re.escape(key)
    text, n = re.subn(pattern, lambda m: m.group(1) + _render(value), original)
    if n != 1:
        raise RuntimeError("%s: expected one '%s = ...' line, found %d" % (path, key, n))

    _originals.setdefault(path, original)
    with open(path, "w", newline="") as handle:
        handle.write(text)

def restore(path):
    """Puts the whole file back the way the run found it, run-wide patches included."""
    path = os.path.abspath(path)
    original = _originals.pop(path, None)
    if original is None:
        return

    with open(path, "w", newline="") as handle:
        handle.write(original)

def restore_all():
    for path in list(_originals):
        restore(path)
