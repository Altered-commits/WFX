# Build Macros

Every preprocessor macro WFX's own build system defines, grouped by what it's for. This does not
list header include guards (`WFX_INC_*`, `WFX_UTILS_*`, ...) - those aren't build configuration,
just the usual one-inclusion-per-TU boilerplate.

---

## Platform detection

Defined in `shared/utils/detection_macro.hpp`, resolved from the compiler's own predefined macros.
Exactly one of the four is set; the header itself `#error`s out if none match.

| Macro | Set when |
|-------|----------|
| `WFX_PLATFORM_LINUX` | `__linux__` defined. Also sets `WFX_PLATFORM_POSIX`. |
| `WFX_PLATFORM_MACOS` | `__APPLE__` defined. Also sets `WFX_PLATFORM_POSIX`. |
| `WFX_PLATFORM_WINDOWS` | `_WIN32` or `_WIN64` defined. |
| `WFX_PLATFORM_POSIX` | Convenience macro, set alongside Linux and macOS (never Windows). |

!!! important
    `WFX_PLATFORM_LINUX` is the only platform with a real, working implementation today.
    `config/config.hpp`/`.cpp` and `http/connection/http_connection_factory.hpp` (which picks the
    connection backend - `os_specific/linux/epoll_connection.hpp` is the only one that exists)
    both `#error` on anything else. Windows/macOS platform macros exist and are partially wired up
    (`utils/diagnostics/crash_tracer.cpp` has per-platform unwind code, `utils/fileops/filesystem.cpp`
    picks the right `stat` mtime field), but there's no epoll-equivalent backend for either yet.

Also in the same header: **`WFX_ARCH_X64`**, **`WFX_ARCH_ARM64`**, **`WFX_ARCH_UNKNOWN`** - CPU
architecture, from `__x86_64__`/`_M_X64` and `__aarch64__`/`_M_ARM64`. Used only by
`crash_tracer.cpp` to pick the right register/unwind layout per platform+arch combination.

---

## Compiler detection

Also in `detection_macro.hpp`, mutually exclusive, `#error`s if none match:

- **`WFX_COMPILER_MSVC`** - `_MSC_VER` and not clang-cl
- **`WFX_COMPILER_CLANG`** - `__clang__`
- **`WFX_COMPILER_INTEL`** - `__INTEL_COMPILER`/`__ICC`
- **`WFX_COMPILER_GCC`** - `__GNUC__`, after clang/Intel are ruled out

Only real usage by name is `shared/utils/hash.hpp` (`WFX_COMPILER_MSVC` picks `_umul128` vs a
`__uint128_t`/manual-multiply fallback for WyHash's mix step). `shared/utils/compiler_macro.hpp`
(next section) branches on the raw predefined macros directly rather than these - the two headers
are independent, not layered on each other.

---

## Build-type / feature flags

| Macro | Set by | Default | Gates |
|-------|--------|---------|-------|
| `WFX_ENGINE_BUILD` | `add_compile_definitions(WFX_ENGINE_BUILD)` in root `CMakeLists.txt`, engine target only | always on for the engine, never set for user-app builds | `shared/utils/memory.hpp` / `include/wfx/memory.hpp`: engine build allocates via `Utils::GetBufferPool()` directly; user-app build goes through the ABI's allocator table instead |
| `WFX_DEBUG` | `$<$<CONFIG:Debug>:WFX_DEBUG>` in `CMakeLists.txt` | only with `CMAKE_BUILD_TYPE=Debug` | nothing yet - defined but no `#ifdef WFX_DEBUG` anywhere in source |
| `WFX_USE_OPENSSL` | `option(WFX_USE_OPENSSL ...)` in `cmake/ssl.cmake`, applied via `target_compile_definitions` | **ON** | selects the OpenSSL backend everywhere TLS/crypto is implemented: `shared/apis/crypto_api.cpp`, `utils/crypto/openssl/*`, `http/ssl/openssl/*`, `http/ssl/http_ssl_factory.hpp`. OpenSSL is the only backend that actually exists right now; `WFX_USE_WOLFSSL`/`WFX_USE_MBEDTLS` are commented-out placeholders in `ssl.cmake` for a future backend swap |
| `WFX_ENABLE_ASAN` | `option(WFX_ENABLE_ASAN ...)` in root `CMakeLists.txt` | **OFF** | not itself a preprocessor macro - only gates `-fsanitize=address,undefined -fno-omit-frame-pointer -g` and the matching link flags. Fails the configure step outright under MSVC. |
| `WFX_TARGET_FLAGS` | `set(WFX_TARGET_FLAGS ... CACHE STRING ...)` in root `CMakeLists.txt` | `-march=native` | also not a preprocessor macro. Replaces the architecture flag(s) passed to the compiler in the Release compile options (GCC, Clang, Intel oneAPI). `-march=native` is correct whenever the build machine is the run machine, which is every self-build (`install.sh`, `--local-*`). `.github/workflows/release_build.yml` overrides it per target instead, e.g. `-DWFX_TARGET_FLAGS="-mcpu=neoverse-n1"`, since those binaries are cross-built on a CI runner for a different machine. The value is split on whitespace into a proper CMake list (`separate_arguments`), so a target needing more than one flag, e.g. `-march=armv8.4-a -mcpu=neoverse-v1`, still reaches the compiler as two separate arguments instead of one. |
| `WFX_STATIC_SSL` | `option(WFX_STATIC_SSL ...)` in `cmake/ssl_openssl.cmake` | **OFF** | not a preprocessor macro either. Controls whether the custom-built OpenSSL is linked into `wfx` statically (`.a`) or dynamically (`.so`). Off by default so local/dev builds (`install.sh`, `--local-*`) stay dynamic, matching what `.github/workflows/audit_check.yml` already expects when it restores that `.so` build cache for test runs. `.github/workflows/release_build.yml` turns it on, so the binaries it publishes are self-contained and don't need this custom OpenSSL build's `.so` files present on the machine that runs them. |
| `WFX_ASAN_BUILD` | added via `add_compile_definitions(WFX_ASAN_BUILD)` when `WFX_ENABLE_ASAN` is on | off unless ASan is on | `utils/diagnostics/crash_tracer.cpp`, inside `CrashTracer::Install()`: skips installing WFX's own `SIGSEGV`/`SIGABRT` crash-dump handler, so ASan's handler stays in control and prints its actual allocation/free-site report instead of a generic WFX crash dump |

!!! important
    `WFX_ENABLE_ASAN` (the CMake option you pass on the command line) and `WFX_ASAN_BUILD` (the
    preprocessor macro it produces) are two different names - don't conflate them in source.

Where `WFX_ENABLE_ASAN` gets turned on:

- `scripts/install.sh --local-debug` (contributor/dev builds) - **on**, paired with
  `CMAKE_BUILD_TYPE=Debug`. Catches dangling-pointer/use-after-free/UB bugs as soon as you hit
  them locally, at the cost of a noticeably slower binary. Use `scripts/install.sh --local-release`
  instead if you need a fast build for perf testing (same checkout, same `build/` dir, ASan off).
- `scripts/install.sh` (real end-user install, no flags) and `scripts/install.sh --local-release`
  - always **off**. Both are the optimized build real users (or perf testing) run.
- `.github/workflows/compile_check.yml` and `tidy_check.yml` - always **on**, also paired with
  `CMAKE_BUILD_TYPE=Debug` (not Release - Release strips symbols and enables LTO, which turns
  ASan's crash traces into useless raw offsets). That build artifact is only ever consumed by
  `audit_check.yml` inside CI, never distributed, so there's no downside to it always being
  instrumented - every audit run catches this whole bug class automatically, with a real stack
  trace when it does.

---

## Portability / utility macros

Compiler-agnostic helpers, all in `shared/utils/compiler_macro.hpp` unless noted, branching
directly on raw compiler predefines (not on `WFX_COMPILER_*` above):

| Macro | Purpose |
|-------|---------|
| `WFX_EXPORT` | Forces a symbol to stay linked and externally visible (exported plugin/user-entry points) |
| `WFX_USED` | Prevents dead-strip/optimize-away without forcing external visibility |
| `WFX_UNREACHABLE` | `__builtin_unreachable()` / `__assume(0)` equivalent per compiler |
| `WFX_FORCE_INLINE` | `inline __attribute__((always_inline))` / `__forceinline` equivalent |
| `WFX_NOINLINE` | `__attribute__((noinline))` / `__declspec(noinline)` equivalent |
| `WFX_ASSUME(cond)` | Optimizer hint; synthesized via `__builtin_unreachable()` on GCC/MinGW since they have no direct equivalent |

Project-specific (not compiler-branching, but worth knowing as a contributor):

- **`WFX_CONCAT(a, b)`** (`include/core/core.hpp`) - token-pasting helper with an extra expansion
  layer, used to build unique `__COUNTER__`-based identifiers in the route/middleware/constructor
  registration macros below.
- **`WFX_CONSTRUCTOR(...)`** (`include/core/constructor.hpp`) - user-facing SDK macro; generates a
  static self-registering struct that runs at static-init time. See `wfx/app.hpp`.
- **`WFX_GET` / `WFX_POST` / `WFX_GET_EX` / `WFX_POST_EX` / `WFX_GROUP_START` / `WFX_GROUP_END`**
  (`include/http/routes.hpp`) - the route-registration DSL, same static-registration pattern.
- **`WFX_MIDDLEWARE(name, ...)` / `WFX_MW_LIST(...)`** (`include/http/middleware.hpp`) -
  middleware-registration DSL, same pattern again.
- **`WFX_TRACE()`** (`utils/diagnostics/crash_tracer.hpp`) - captures a `__func__`/`__FILE__`/`__LINE__`
  frame into the crash-tracer's ring buffer. Used at ~30 call sites across
  `os_specific/linux/epoll_connection.cpp` and `engine/core_engine.cpp`.
- **`WFX_CHECKPOINT(label)`** (`utils/diagnostics/crash_tracer.hpp`) - updates the current
  `WFX_TRACE()` frame's label. Defined but currently has zero call sites - reserved for future use,
  same as `WFX_DEBUG`.
- **`WFX_IS_TTY()`**, **`WFX_STDOUT_WRITE(data, len)`**, **`WFX_LOCALTIME(tm, tt)`**
  (`utils/diagnostics/logger.hpp`) - thin POSIX wrappers (`isatty`, a retrying write, `localtime_r`)
  used by the logger. POSIX-only, unguarded by `WFX_PLATFORM_POSIX` since the engine only targets
  Linux today anyway.
