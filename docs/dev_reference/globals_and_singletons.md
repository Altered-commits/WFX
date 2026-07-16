# Globals and Singletons

WFX uses plain globals with free functions instead of Meyers singletons. Each global lives in a `.cpp` file inside an anonymous namespace, making it inaccessible from outside that translation unit. The only way to reach it is through its `Get...()` function.

This avoids the thread-safe initialization guard that Meyers singletons emit on every call, and makes initialization order explicit rather than lazy.

---

## The functions

### `GetLogger()`

```cpp
// Namespace: WFX::Utils
// Defined in: utils/diagnostics/logger.cpp
WFX::Utils::Logger& GetLogger() noexcept;
```

Returns the per-worker logger instance. Handles stdout output, file rotation, ANSI colors, and timestamps. Not thread-safe by design since each worker is single-threaded.

`GetLogger()` is the only function here that uses a heap-allocated instance instead of a plain static:

```cpp
Logger& GetLogger() noexcept
{
    static Logger* __GlobalLogger = new Logger();
    return *__GlobalLogger;
}
```

The reason is static destruction order. When the process exits, globals across translation units are destroyed in an undefined order. Several globals call `GetLogger()` in their destructors (for example `BufferPool` logs its shutdown metrics). If `__GlobalLogger` were a plain static and got destroyed before `BufferPool`, those destructor calls would use a dangling reference and crash. Heap-allocating the logger means its destructor never runs, so it is always valid regardless of what order other globals destruct. The OS reclaims the memory on exit.

---

### `GetConfig()`

```cpp
// Namespace: WFX::Core
// Defined in: config/config.cpp
WFX::Core::Config& GetConfig() noexcept;
```

Returns the global config instance. Populated by `LoadCoreSettings()` and `LoadFinalSettings()` in the master process before the fork. After the fork, all workers inherit the same config values and treat it as read-only. Never write to config from worker code.

---

### `GetRandomPool()`

```cpp
// Namespace: WFX::Utils
// Defined in: utils/crypto/hash.cpp
WFX::Utils::RandomPool& GetRandomPool() noexcept;
```

Returns the cryptographically secure random byte pool. Backed by `getrandom()` on Linux and `BCryptGenRandom` on Windows, with a `/dev/urandom` fallback for older kernels. The pool pre-fills 1MB of random bytes at construction and refills on demand.

The SSL key is generated from this pool during construction and cached. All workers inherit the same SSL key after the fork via `GetSSLKey()`.

If workers need crypto random bytes after the fork, `GetBytes()` is safe to call per-worker since each worker has its own pool instance.

---

### `GetBufferPool()`

```cpp
// Namespace: WFX::Utils
// Defined in: utils/pool/buffer_pool.cpp
WFX::Utils::BufferPool& GetBufferPool() noexcept;
```

Returns the per-worker TLSF memory pool. Must be explicitly initialized after the fork via `Init()` before any allocations are made. All connection buffers, read/write buffers, and response buffers are allocated from this pool.

The pool grows automatically if exhausted, but under normal load this should not happen. At process exit, it logs allocation statistics.

Do not call `Init()` more than once per worker. Calling it twice is a fatal error.

---

### `GetFileCache()`

```cpp
// Namespace: WFX::Utils
// Defined in: utils/fileops/filecache.cpp
WFX::Utils::FileCache& GetFileCache() noexcept;
```

Returns the per-worker LFU file descriptor cache. Caches open file descriptors and their sizes to avoid repeated `open()` and `stat()` syscalls for static file serving. Must be initialized after the fork via `Init()` with a capacity value from config.

Used by the epoll connection handler when serving files. User code does not interact with this directly.

---

### `GetTemplateEngine()`

```cpp
// Namespace: WFX::Core
// Defined in: engine/template_engine.cpp
WFX::Core::TemplateEngine& GetTemplateEngine() noexcept;
```

Returns the template engine instance. Template compilation (`PreCompileTemplates()` and `LoadDynamicTemplatesFromLib()`) happens in the master process before the fork. Workers inherit the compiled template state and call `GetTemplate()` at request time to serve pre-compiled output.

Do not call the compilation functions from worker code. They are master-only.

---

### `GetMasterState()`

```cpp
// Namespace: WFX::Http
// Defined in: http/common/http_master_state.cpp
WFX::Http::WFXMasterState& GetMasterState() noexcept;
```

Returns the master process lifecycle state. Contains `shouldStop`, `enginePtr`, `workerPids`, and `workerPGID`. Unlike the other globals, this one is shared in the sense that master and workers each have their own copy of the struct after the fork, and they use it independently.

The master uses `workerPids` and `workerPGID` for process group management and shutdown. Each worker uses `enginePtr` to call `engine.Stop()` from its signal handler. `shouldStop` is used by the master's wait loop.

---

## `MetricTracer`

`MetricTracer` is a pure static class with no instance and no `Get` function. It manages a shared `mmap` region allocated before the fork. See the **[Metrics and Observability](./metrics_and_observability.md)** page for details.

---

## Initialization order

The order in which these are initialized matters. The master process follows this sequence:

1. `GetLogger()` - available immediately at program start (heap allocated, constructed on first call)
2. `GetConfig()` - constructed at program start, populated via `LoadCoreSettings()`
3. `GetRandomPool()` - constructed at program start, generates SSL key in constructor
4. `GetTemplateEngine()` - constructed at program start, compiled before fork
5. `MetricTracer::Create()` - called explicitly before fork
6. `fork()`
7. Per-worker: `GetBufferPool().Init()` and `GetFileCache().Init()`

`GetMasterState()` is available throughout. `GetLogger()` is the only one safe to call from a destructor of any other global.