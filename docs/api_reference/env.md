# Environment Variables

WFX can optionally load a `.env` file into the process environment before your workers start, and provides a handful of typed getters over `std::getenv` for the obviously-common cases. Anything more specific than that (floats, enums, lists, custom validation) is left to you, plain `std::getenv` still works directly for that.

!!! important
    ```cpp
    #include <wfx/utils/env.hpp>
    ```

---

## Loading a `.env` file

Point at it from `wfx.toml`:

```toml
[ENV]
env_path = "..." # Path to a .env file
```

!!! important
    The `[ENV]` table and its `env_path` key are read unconditionally from `wfx.toml`, startup fails if either is missing entirely, even though the file at that path is allowed to not exist (see below). Every project scaffolded by `wfx new` already includes this block by default; if you hand-write a minimal `wfx.toml`, don't drop it.

Loading happens once, in the master process, before workers are spawned:

- If the file at `env_path` doesn't exist, can't be opened, or fails the permission checks below, loading is silently skipped. This is not a fatal error, the server still starts.
- If it exists and passes, every `KEY=VALUE` line is applied to the process environment, and workers inherit that environment on fork. Every getter below just works inside every route handler with no extra wiring.

### File format

Line-based `KEY=VALUE` pairs:

```
# comments start with '#'
DATABASE_URL=postgres://localhost/mydb
API_KEY="abc123"
```

- Leading/trailing whitespace on both key and value is trimmed.
- A value fully wrapped in matching `"..."` or `'...'` has the quotes stripped.
- Blank lines and `#`-prefixed lines are ignored.
- No `export KEY=VALUE` syntax, no multi-line values, no `${OTHER_VAR}` interpolation inside the file itself.

### Permissions

On non-Windows systems, the `.env` file must be owned by the user running WFX and have mode `600` (owner read/write only, nothing for group or other). A file failing either check is treated the same as a missing file: skipped, not fatal.

!!! note
    An existing process/shell environment variable is never overwritten by a `.env` value with the same key. Real environment (shell, systemd, Docker, k8s secrets, etc.) always wins over `.env` file contents.

---

## Reading a variable

```cpp
std::string_view GetEnvString(const char* name, std::string_view defaultValue = {}) noexcept;
bool              GetEnvBool(const char* name, bool defaultValue) noexcept;
std::int64_t      GetEnvInt(const char* name, std::int64_t defaultValue) noexcept;
```

All three return `defaultValue` on an unset variable **and** on a value that doesn't parse for the requested type, they don't distinguish the two.

- **`GetEnvString`**: the raw value, or `defaultValue` if unset.
- **`GetEnvBool`**: accepts `"1"`/`"0"` and `"true"`/`"false"` (case-insensitive). Anything else falls back to `defaultValue`.
- **`GetEnvInt`**: parses the entire value as a base-10 integer. Trailing garbage after the number (e.g. `"8080x"`), or an empty string, also falls back to `defaultValue`, not just an unset variable.

For anything these three don't cover, `std::getenv` works exactly like it does in any C++ program.

**Example (global scope, recommended)**: `.env` loading happens in the master process before workers fork, so by the time a worker's shared library is loaded and its static/global initializers run, the environment is already fully populated. That makes namespace-scope initialization the natural place to read a variable once, instead of re-reading it on every request:

```cpp
static const bool debugMode      = WFX::GetEnvBool("DEBUG", false);
static const std::int64_t port   = WFX::GetEnvInt("PORT", 8080);
static const auto dbUrl          = WFX::GetEnvString("DATABASE_URL", "postgres://localhost/dev");
```

**Example (inside a handler)**:
```cpp
WFX_GET("/db-status", [](WFX::Request req, WFX::Response res) {
    auto dbUrl = WFX::GetEnvString("DATABASE_URL");

    if(dbUrl.empty()) {
        res.Status(WFX::HttpStatus::INTERNAL_SERVER_ERROR).SendText("DATABASE_URL not set");
        return;
    }

    res.SendText(dbUrl);
});
```

!!! important
    `std::getenv`, and therefore every getter above, isn't safe to call concurrently with anything that mutates the environment (`setenv`/`putenv`/`unsetenv`). WFX itself only ever mutates the environment once, in the master process, before any worker starts handling requests, so both patterns above are safe as long as your own code doesn't call `setenv`/`putenv`/`unsetenv` from a route handler at runtime.

---

See [wfx.toml `[ENV]`](../core_concepts/wfx_toml.md#env) for the config reference this page builds on.
