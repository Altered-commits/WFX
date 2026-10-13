# Build Macros

Every preprocessor macro WFX's own build system defines, grouped by what it's for. This does not
list header include guards (`WFX_INC_*`, `WFX_UTILS_*`, ...) - those aren't build configuration,
just the usual one-inclusion-per-TU boilerplate.

---

## Platform and compiler

`shared/utils/detection_macro.hpp` `#error`s unless the target is Linux (`__linux__`) and the
compiler is GCC or Clang (`__GNUC__`, which Clang defines too). WFX builds and runs on Linux only,
with `os_specific/linux/epoll_connection.hpp` as its one connection backend, and the root
`CMakeLists.txt` refuses to configure anywhere else.

The same header defines **`WFX_ARCH_X64`**, **`WFX_ARCH_ARM64`** or **`WFX_ARCH_UNKNOWN`**, from
`__x86_64__` and `__aarch64__`. `crash_tracer.cpp` uses them to pick the register layout it dumps.

---

## Build-type / feature flags

| Macro | Set by | Default | Gates |
|-------|--------|---------|-------|
| `WFX_ENGINE_BUILD` | `target_compile_definitions(wfx PRIVATE ...)` in root `CMakeLists.txt`, so the `wfx` target only (`wfx_header_check` compiles without it, the way a user `.so` does) | always on for the engine, never set for user-app builds | `shared/utils/memory.hpp` / `include/wfx/memory.hpp`: engine build allocates via `Utils::GetBufferPool()` directly; user-app build goes through the ABI's allocator table instead |
| `WFX_DEBUG` | `$<$<CONFIG:Debug>:WFX_DEBUG>` in `CMakeLists.txt` | only with `CMAKE_BUILD_TYPE=Debug` | nothing yet - defined but no `#ifdef WFX_DEBUG` anywhere in source |
| `WFX_USE_OPENSSL` | `option(WFX_USE_OPENSSL ...)` in `cmake/ssl.cmake`, applied via `target_compile_definitions` | **ON** | selects the OpenSSL backend everywhere TLS/crypto is implemented: `shared/apis/crypto_api.cpp`, `utils/crypto/openssl/*`, `http/ssl/openssl/*`, `http/ssl/http_ssl_factory.hpp`. OpenSSL is the only backend that actually exists right now; `WFX_USE_WOLFSSL`/`WFX_USE_MBEDTLS` are commented-out placeholders in `ssl.cmake` for a future backend swap |
| `WFX_ENABLE_ASAN` | `option(WFX_ENABLE_ASAN ...)` in root `CMakeLists.txt` | **OFF** | not itself a preprocessor macro - only gates `-fsanitize=address,undefined -fno-omit-frame-pointer -g` and the matching link flags. |
| `WFX_TARGET_FLAGS` | `set(WFX_TARGET_FLAGS ... CACHE STRING ...)` in root `CMakeLists.txt` | `-march=native` | also not a preprocessor macro. Replaces the architecture flag(s) passed to the compiler in the Release compile options (GCC, Clang, Intel oneAPI). `-march=native` is correct whenever the build machine is the run machine, which is every self-build (`install.sh`, `--local-*`). `.github/workflows/release_build.yml` overrides it per target instead, e.g. `-DWFX_TARGET_FLAGS="-mcpu=neoverse-n1"`, since those binaries are cross-built on a CI runner for a different machine. The value is split on whitespace into a proper CMake list (`separate_arguments`), so a target needing more than one flag, e.g. `-march=armv8.4-a -mcpu=neoverse-v1`, still reaches the compiler as two separate arguments instead of one. |
| `WFX_STATIC_SSL` | `option(WFX_STATIC_SSL ...)` in `cmake/ssl_openssl.cmake` | **OFF** | not a preprocessor macro either. Controls whether the custom-built OpenSSL is linked into `wfx` statically (`.a`) or dynamically (`.so`). Off by default so local/dev builds (`install.sh`, `--local-*`) stay dynamic, matching what `.github/workflows/audit_check.yml` already expects when it restores that `.so` build cache for test runs. `.github/workflows/release_build.yml` turns it on, so the binaries it publishes are self-contained and don't need this custom OpenSSL build's `.so` files present on the machine that runs them. |
| `WFX_ASAN_BUILD` | added via `add_compile_definitions(WFX_ASAN_BUILD)` when `WFX_ENABLE_ASAN` is on | off unless ASan is on | `utils/diagnostics/crash_tracer.cpp`, inside `CrashTracer::Install()`: skips installing WFX's own `SIGSEGV`/`SIGABRT` crash-dump handler, so ASan's handler stays in control and prints its actual allocation/free-site report instead of a generic WFX crash dump. Also `utils/diagnostics/logger.cpp`, inside `GetLogger()`: calls `__lsan_ignore_object` on its one intentional heap-allocated-and-never-freed singleton, so LeakSanitizer ignores just that allocation instead of leak detection needing to be disabled process-wide, see [Testing](testing.md). Also `utils/pool/buffer_pool.cpp`: `BufferPool` allocates, reallocates and frees through `malloc`/`realloc`/`free` instead of TLSF, so ASan sees every allocation (TLSF would hide use-after-free inside its own pool) |

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

Attribute helpers in `shared/utils/compiler_macro.hpp`, for GCC and Clang:

| Macro | Purpose |
|-------|---------|
| `WFX_EXPORT` | Forces a symbol to stay linked and externally visible (exported plugin/user-entry points) |
| `WFX_USED` | Prevents dead-strip/optimize-away without forcing external visibility |
| `WFX_UNREACHABLE` | `__builtin_unreachable()` |
| `WFX_FORCE_INLINE` | `inline __attribute__((always_inline))` |
| `WFX_NOINLINE` | `__attribute__((noinline))` |
| `WFX_ASSUME(cond)` | Optimizer hint: `__builtin_assume` on Clang, synthesized via `__builtin_unreachable()` on GCC, which has no direct equivalent |

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
- **`WFX_TRACE()`** (`utils/diagnostics/crash_tracer.hpp`) - pushes a `__func__`/`__FILE__`/`__LINE__`
  frame onto the crash tracer's 48-deep frame stack, popped on scope exit. Used at ~45 call sites
  across `os_specific/linux/epoll_connection.cpp` and `engine/core_engine.cpp`.
- **`WFX_CHECKPOINT(label)`** (`utils/diagnostics/crash_tracer.hpp`) - updates the current
  `WFX_TRACE()` frame's label. Defined but currently has zero call sites - reserved for future use,
  same as `WFX_DEBUG`.
- **`WFX_IS_TTY()`**, **`WFX_STDOUT_WRITE(data, len)`**, **`WFX_LOCALTIME(tm, tt)`**
  (`utils/diagnostics/logger.hpp`) - thin POSIX wrappers (`isatty`, a retrying write, `localtime_r`)
  used by the logger.
