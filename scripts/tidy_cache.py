#!/usr/bin/env python3
"""
Minimal cache wrapper around clang-tidy, invoked by scripts/tidy.sh.

Usage:
    tidy_cache.py <real-clang-tidy-path> <clang-tidy-args...> <file>

Behaviour is designed to be a drop-in replacement for calling clang-tidy-
-directly: stdout+stderr are combined and printed exactly as clang-tidy would-
-print them, and the process exits with clang-tidy's own exit code, whether-
-served from cache or freshly run.

Cache key = sha256 of:
  - the real clang-tidy binary's own `--version` string (so a different
    clang-tidy build/version never reuses another version's cached result)
  - clang-tidy's fully resolved effective config for this file, via
    `--dump-config` (covers .clang-tidy contents + built-in defaults)
  - the preprocessed translation unit (source + every transitively included
    header, exactly as the compiler would see it - NOT just the file's own
    mtime/content, so an unrelated header edit still invalidates the cache)
  - the clang-tidy arguments themselves (checks, flags, etc.)

If anything about computing that key fails for any reason, we just skip the-
-cache and run clang-tidy directly, so a bug here can only ever cost time

Cache dir: $CTCACHE_DIR (default: ~/.wfx/tidy_cache). Each cache entry-
-is a small file: first line is the exit code, the rest is the exact-
-stdout+stderr clang-tidy produced.
"""

import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile

def eprint(*args):
    print(*args, file=sys.stderr)

def load_compile_db(build_dir):
    path = os.path.join(build_dir, "compile_commands.json")
    with open(path) as f:
        return json.load(f)

def find_compile_command(db, target_file):
    target_real = os.path.realpath(target_file)
    for entry in db:
        entry_file = entry["file"]
        if not os.path.isabs(entry_file):
            entry_file = os.path.join(entry.get("directory", ""), entry_file)
        try:
            if os.path.realpath(entry_file) != target_real:
                continue
        except OSError:
            continue
        if "arguments" in entry:
            return list(entry["arguments"])
        return shlex.split(entry["command"])
    return None

def to_preprocess_args(compile_args):
    """-c -> -E, drop/replace -o <out> with stdout ('-')."""
    args = list(compile_args)
    out = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "-c":
            out.append("-E")
        elif arg == "-o":
            out.append("-o")
            out.append("-")
            i += 1  # skip the original output path
        else:
            out.append(arg)
        i += 1
    return out

def compute_digest(real_clang_tidy, tidy_args, build_dir, target_file):
    version = subprocess.run(
        [real_clang_tidy, "--version"], capture_output=True, text=True, check=True
    ).stdout

    db = load_compile_db(build_dir)
    compile_args = find_compile_command(db, target_file)
    if compile_args is None:
        raise RuntimeError(f"no compile_commands.json entry for {target_file}")

    preprocessed = subprocess.run(
        to_preprocess_args(compile_args), capture_output=True, check=True
    ).stdout

    dump_config = subprocess.run(
        [real_clang_tidy, "-p", build_dir, "--dump-config", target_file],
        capture_output=True, check=True
    ).stdout

    h = hashlib.sha256()
    h.update(version.encode("utf-8"))
    h.update(dump_config)
    h.update(preprocessed)
    h.update(" ".join(tidy_args).encode("utf-8"))
    return h.hexdigest()

def cache_path(digest):
    # Lives under ~/.wfx (same root as the rest of WFX's user-owned state:-
    # -bin/, daemons/, src/) so 'rm -rf ~/.wfx' erases everything, no stray-
    # -dirs left behind anywhere else on disk.
    cache_dir = os.environ.get(
        "CTCACHE_DIR", os.path.join(os.path.expanduser("~"), ".wfx", "tidy_cache")
    )
    return os.path.join(cache_dir, digest[:2], digest[2:])

def read_cache(path):
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as f:
        data = f.read()
    newline = data.find(b"\n")
    if newline == -1:
        return None
    try:
        exit_code = int(data[:newline])
    except ValueError:
        return None
    return exit_code, data[newline + 1:]

def write_cache(path, exit_code, output_bytes):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp_path = tempfile.mkstemp(dir=os.path.dirname(path))
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(f"{exit_code}\n".encode("utf-8"))
            f.write(output_bytes)
        os.replace(tmp_path, path)  # atomic, safe even with concurrent writers
    except BaseException:
        os.unlink(tmp_path)
        raise

def run_real_clang_tidy(argv):
    proc = subprocess.run(argv, capture_output=True)
    output = proc.stdout + proc.stderr
    sys.stdout.buffer.write(output)
    return proc.returncode, output

def main():
    if len(sys.argv) < 3:
        eprint("Usage: tidy_cache.py <real-clang-tidy> <clang-tidy-args...> <file>")
        return 1

    real_clang_tidy = sys.argv[1]
    rest = sys.argv[2:]
    target_file = rest[-1]
    full_argv = [real_clang_tidy] + rest

    build_dir = None
    if "-p" in rest:
        build_dir = rest[rest.index("-p") + 1]

    digest = None
    if build_dir and os.environ.get("CTCACHE_DISABLE", "0") not in ("1", "true", "yes"):
        try:
            digest = compute_digest(real_clang_tidy, rest[:-1], build_dir, target_file)
        except Exception as error:
            eprint(f"[tidy_cache] Skipping cache for this file, hashing failed: {error}")
            digest = None

    if digest:
        path = cache_path(digest)
        cached = read_cache(path)
        if cached is not None:
            exit_code, output = cached
            sys.stdout.buffer.write(output)
            return exit_code

    exit_code, output = run_real_clang_tidy(full_argv)

    if digest:
        try:
            write_cache(cache_path(digest), exit_code, output)
        except OSError as error:
            eprint(f"[tidy_cache] Failed to write cache entry: {error}")

    return exit_code

if __name__ == "__main__":
    sys.exit(main())