# Codebase Structure

This page describes the top-level layout of the WFX repository.

---

## Source folders

- `cli/`  
    Entry point of the entire engine. Contains `main.cpp` and all CLI commands such as `new`, `run`, etc. This is where the server lifecycle starts.

- `cmake/`  
    CMake sub-logic files. Build logic is split into separate files here to keep the root `CMakeLists.txt` clean and readable.

- `config/`  
    Config loading using **toml++**. Parses `wfx.toml` into typed structs used across the engine.

- `engine/`  
    The core engine and template engine. This is the main orchestrator that ties the connection layer, routing, middleware, and user code together.

- `http/`  
    All HTTP-related engine code. Covers middleware, SSL, IP limiting, header parsing, request & response, and so on.

- `include/`  
    User-facing headers. Everything the user includes in their project lives here. The engine rarely touches this folder.

- `os_specific/`  
    Platform-specific I/O backends. Each supported OS implements the same connection handler interface here. The engine calls the interface, never the platform code directly.

- `shared/`  
    Code shared between the engine and user code. Contains ABI-stable structs and types that cross the boundary between the engine and the loaded user library.

- `tests/`  
    Adversarial audit suites, each a Python harness driving a real, compiled WFX
    server (or `HttpEndpoint` client) as a black box. Not unit tests, these boot
    the server, throw a large corpus of legal and malformed/hostile input at it,
    and assert on observed behavior (crashes, hangs, mis-framing, smuggling,
    connection poisoning, and similar).
    - `base_audit/` - phased correctness and security audit of the inbound server.
    - `endpoint_audit/` - adversarial audit of the outbound `WFX::HttpEndpoint` client against a hostile upstream.
    - `tls_audit/` - adversarial audit of the outbound client over TLS (untrusted/hostname-mismatched/expired certs, protocol downgrade).
    - `crypto_audit/` - correctness audit of `wfx/utils/crypto.hpp` (hashing, HMAC, AEAD, KDFs, CSPRNG) against Python stdlib oracles where one exists.
    - `ip_audit/` - adversarial audit of real-IP resolution and the connection/rate limiters it feeds.
    - `common/` - the shared `Suite`/`Report`/`Server`/`net`/`logs`/`term` package every audit above is built on, see [Testing](testing.md).

- `utils/`  
    Internal engine utilities. Not exposed to user code. Contains the logger, buffer pool, file cache, crash tracer, metric tracer, and other engine-side tools.

---

## Docs and config

- `docs/`  
    MkDocs content. All markdown pages, CSS, and assets for this documentation site live here.

- `mkdocs.yml`  
    MkDocs entry point at the repository root.

- `CMakeLists.txt`  
    Main build file for the entire project.

---

## Repository files

- `.github/`  
    GitHub Actions workflows, under `workflows/`.
    - `entry.yml` - The only workflow actually triggered on push/PR. Orchestrates the five reusable workflows below: filter, then format, then compile, then audit and tidy in parallel (both gated on compile succeeding, not on each other). Also reports one collected status for branch protection.
    - `filter_check.yml` - Reusable, called by `entry.yml`. Decides whether CI should run at all based on which files changed, using `.ciignore`.
    - `format_check.yml` - Reusable, called by `entry.yml` after the filter passes. Validates code formatting using `scripts/format.sh --dry-run`.
    - `compile_check.yml` - Reusable, called by `entry.yml` after formatting passes. Checks for successful compilation of WFX, always built `Debug` with `-DWFX_ENABLE_ASAN=ON` (see [Build Macros](build_macros.md)) since the artifact never leaves CI - `Debug` keeps full symbols so ASan crash traces are actually readable.
    - `audit_check.yml` - Reusable, called by `entry.yml` after compile passes. Never builds `wfx` itself: downloads the ASan-instrumented binary `compile_check.yml` already uploaded as an artifact, restores that same job's `build/` cache for the custom OpenSSL `.so`s `wfx` links against (headers come from its own checkout, they're tracked source), then runs the five test audits (`base`, `endpoint`, `tls`, `crypto`, `ip`) as parallel matrix jobs via `tests/run_audits.sh`.
    - `tidy_check.yml` - Reusable, called by `entry.yml` after compile passes, in parallel with `audit_check.yml`. Runs `scripts/tidy.sh` (clang-tidy static analysis).
    - `docs_build.yml` - Independent, triggers on push to `main`. Builds and deploys this documentation site.

- `scripts/`  
    Shell scripts for project tooling.
    - `install.sh` - Installs WFX to `~/.wfx`, builds / updates from source, and adds the binary to PATH. `--local-debug` (contributor mode) symlinks `~/.wfx/src` to the checkout and builds Debug with ASan+UBSan on; `--local-release` does the same symlink but builds an optimized Release with sanitizers off (perf testing); the plain end-user path (no flags) does a real clone and an optimized Release build.
    - `uninstall.sh` - Removes `~/.wfx` entirely and cleans up PATH entries from shell configs.
    - `format.sh` - Runs clang-format across the codebase. Supports `--dry-run` for CI validation and `--files` for targeted formatting.
    - `tidy.sh` - Runs clang-tidy static analysis, self-caching results (`py/tidy_cache.py`) and parallelized across jobs. Supports `--changed` (only files changed vs `main`) and `--fix` (apply auto-fixes).

- `.ciignore`  
    Defines file patterns that do not trigger CI when changed. Works together with `filter_check.yml`. If every file changed in a commit matches a pattern in this file, the build is skipped.

- `.clang-format`  
    clang-format style rules, enforced by `scripts/format.sh` and the `format_check.yml` workflow.

- `.gitignore`  
    Standard git ignore rules.

- `.todo`  
    Internal development notes. Plain text, not formal issue tracking.

- `CONTRIBUTING.md`  
    Contribution guidelines.

- `LICENSE`  
    Project license.

- `NOTICE`  
    Top-level copyright notice, points to `THIRD_PARTY_NOTICES.md` for third-party attributions.

- `THIRD_PARTY_NOTICES.md`  
    Full third-party license texts and attributions. Covers toml++, TLSF, and OpenSSL.

- `README.md`  
    Project readme.