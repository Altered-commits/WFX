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

- `test/`  
    Not in use yet.

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
    GitHub Actions workflows.
    - `ci_filter.yml` - Reusable workflow that checks whether a commit should trigger CI based on which files changed.
    - `docs_build.yml` - Builds and deploys this documentation site.
    - `compile_check.yml` - Checks for successful compilation of WFX.
    - `format_check.yml` - Validates code formatting using `scripts/format.sh --dry-run`.

- `scripts/`  
    Shell scripts for project tooling.
    - `install.sh` - Installs WFX to `~/.wfx`, builds / updates from source, and adds the binary to PATH.
    - `uninstall.sh` - Removes `~/.wfx` entirely and cleans up PATH entries from shell configs.
    - `format.sh` - Runs clang-format across the codebase. Supports `--dry-run` for CI validation and `--files` for targeted formatting.

- `.ciignore`  
    Defines file patterns that do not trigger CI when changed. Works together with `ci_filter.yml`. If every file changed in a commit matches a pattern in this file, the build is skipped.

- `.gitignore`  
    Standard git ignore rules.

- `.todo`  
    Internal development notes. Plain text, not formal issue tracking.

- `CONTRIBUTING.md`  
    Contribution guidelines.

- `LICENSE`  
    Project license.

- `NOTICES.md`  
    Third-party library notices. Covers toml++, TLSF, and OpenSSL.

- `README.md`  
    Project readme.