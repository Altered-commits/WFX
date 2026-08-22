# Codebase Structure

This page describes the top-level layout of the WFX repository.

---

## Source folders

- `cli/`  
    Entry point of the entire engine. Contains `main.cpp` and all CLI commands such as `new`, `run`, etc. This is where the server lifecycle starts.

- `cmake/`  
    CMake sub-logic files. Build logic is split into separate files here to keep the root `CMakeLists.txt` clean and readable.

- `config/`  
    Config loading using **toml++**. Parses a project's `wfx.<env>.toml` into typed structs used across the engine.

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
    - `endpoint_audit/` - adversarial audit of the raw outbound `WFX::Endpoint<>` primitive against a hostile upstream.
    - `client_audit/` - adversarial audit of the shipped protocol clients (`HttpEndpoint`, `SmtpEndpoint`, `PostgresEndpoint`) against hostile upstreams.
    - `tls_audit/` - adversarial audit of the outbound client over TLS (untrusted/hostname-mismatched/expired certs, protocol downgrade).
    - `crypto_audit/` - correctness audit of `wfx/utils/crypto.hpp` (hashing, HMAC, AEAD, KDFs, CSPRNG) against Python stdlib oracles where one exists.
    - `ip_audit/` - adversarial audit of real-IP resolution and the connection/rate limiters it feeds.
    - `interop_audit/` - non-adversarial audit proving the same protocol clients work against real, spec-compliant upstreams (Postgres and SMTP in Docker, HTTP against a second real WFX server), see [Testing](testing.md).
    - `common/` - the shared `Suite`/`Report`/`Server`/`net`/`logs`/`term` package every audit above is built on, see [Testing](testing.md).

- `utils/`  
    Internal engine utilities. Not exposed to user code. Contains the logger, buffer pool, file cache, crash tracer, metric tracer, and other engine-side tools.

---

## Naming conventions

Global namespaces: `WFX::CLI`, `WFX::Core`, `WFX::Http`, `WFX::OSSpecific`, `WFX::Shared`, `WFX::Utils`.

User-facing code lives in `include/`. Headers meant to be included directly by users go under
`include/wfx/`. If you are adding a new user-facing feature, the public header belongs there.
Namespaces inside user-facing headers follow the same `WFX::` convention as everything else.

| Kind | Convention | Example |
|------|------------|---------|
| Namespaces / Classes / Structs / Enums / Function identifiers | `PascalCase` | `class Timer` |
| Variables / Function parameters / Locals / Public member variables | `camelCase` | `currentTick` |
| Private / Protected member variables | `camelCase_` (trailing underscore) | `currentTick_` |
| Constants / Enum values | `SCREAMING_SNAKE_CASE` | `MAX_TICK` |
| Global variables (non-`constexpr`) | `GlobalPascalCase` | `GlobalLogger` |
| Macros | `SCREAMING_SNAKE_CASE` | `WFX_IS_TTY` |

```cpp
namespace WFX::Utils {

enum class TimerType {
    MILLISECONDS,
    SECONDS,
    MINUTES
};

class Timer {
public:
    void StartTimer(std::uint64_t timeout, TimerType type);

private:
    std::uint64_t currentTick = 0;
    static constexpr int MAX_TICK = 1000;
};

} // namespace WFX::Utils
```

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
    - `format_check.yml` - Reusable, called by `entry.yml` after the filter passes. Validates code formatting using `scripts/format.sh` (dry run by default, same convention as `tidy.sh`).
    - `compile_check.yml` - Reusable, called by `entry.yml` after formatting passes. Checks for successful compilation of WFX, always built `Debug` with `-DWFX_ENABLE_ASAN=ON` (see [Build Macros](build_macros.md)) since the artifact never leaves CI - `Debug` keeps full symbols so ASan crash traces are actually readable.
    - `audit_check.yml` - Reusable, called by `entry.yml` after compile passes. Never builds `wfx` itself: downloads the ASan-instrumented binary `compile_check.yml` already uploaded as an artifact, restores that same job's `build/` cache for the custom OpenSSL `.so`s `wfx` links against (headers come from its own checkout, they're tracked source), then runs all seven test audits (`base`, `endpoint`, `client`, `tls`, `crypto`, `ip`, `interop`) as parallel matrix jobs via `tests/run_audits.sh`. `interop` needs Docker, which comes preinstalled on the runner.
    - `tidy_check.yml` - Reusable, called by `entry.yml` after compile passes, in parallel with `audit_check.yml`. Runs `scripts/tidy.sh` (clang-tidy static analysis).
    - `docs_build.yml` - Independent, triggers on push to `main`. Builds and deploys this documentation site.
    - `release_build.yml` - Independent, triggers on pushing a version tag. A guard job first checks the tagged commit is actually reachable from `main`, refusing the release otherwise. The build matrix then compiles the engine once per supported CPU target (see `WFX_TARGET_FLAGS` in [Build Macros](build_macros.md)), with `WFX_STATIC_SSL=ON` so each binary links its own OpenSSL statically instead of depending on that CI runner's `.so` files, and a final job publishes every target's binary plus its checksum as assets on a GitHub Release matching the tag.

- `scripts/`  
    Shell scripts for project tooling.
    - `install.sh` - Installs WFX to `~/.wfx`, builds / updates from source, and adds the binary to PATH. `--local-debug` (contributor mode) symlinks `~/.wfx/src` to the checkout and builds Debug with ASan+UBSan on; `--local-release` does the same symlink but builds an optimized Release with sanitizers off (perf testing); the plain end-user path (no flags) does a real clone and an optimized Release build.
    - `uninstall.sh` - Removes `~/.wfx` entirely and cleans up PATH entries from shell configs.
    - `format.sh` - Runs clang-format across the codebase. Dry run by default (used for CI validation); `--fix` applies formatting in-place, `--files` targets specific files.
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