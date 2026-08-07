# Contributing to WFX

I'm surprised you actually decided to contribute to this 'whatever' of a project. Thank you 'user'.
So some guidelines and stuff to get you an idea (I don't even remember half the stuff but yeah).

---

### Setup

Fork the repo, then clone your fork checked out to `dev` and build:

```bash
git clone -b dev https://github.com/<you>/WFX.git
cd WFX
bash scripts/install.sh --local-debug
```

See https://altered-commits.github.io/WFX/getting_started/installation/ for what that does under the hood. `--local-debug` builds straight into your checkout's own `build/` (via a `~/.wfx/src` symlink back to it, so nothing is duplicated) and moves the resulting binary to `~/.wfx/bin/wfx` on your PATH. After editing, either run `./wfx` directly from the checkout root, or re-run `bash scripts/install.sh --local-debug` to refresh the PATH binary.

`--local-debug` builds Debug (full symbols, no optimization) with AddressSanitizer + UndefinedBehaviorSanitizer on, so they catch dangling-pointer/use-after-free/UB bugs as soon as you hit them locally instead of only in CI. That makes the dev binary noticeably slower than a real install - if you need a fast build for perf testing, use `bash scripts/install.sh --local-release` instead (same checkout, same `build/` dir, optimized Release with sanitizers off).

`main` and `dev` are protected. Work freely in your fork or personal branch.

---

### Code Organization

Global namespaces:

- `WFX::CLI`
- `WFX::Core`
- `WFX::Http`
- `WFX::OSSpecific`
- `WFX::Shared`
- `WFX::Utils`

User-facing code lives in `include/`. Headers meant to be included directly by users go under `include/wfx/`. If you are adding a new user-facing feature, the public header belongs there. Namespaces inside user-facing headers should follow the same `WFX::` convention (I'm too lazy to explain rn, just check it out once)

---

### Coding Conventions

- **Namespaces / Classes / Structs / Enums / Function identifiers:** `PascalCase`
- **Variables / Function parameters / Locals / Public member variables:** `camelCase`
- **Private / Protected member variables:** `camelCase_` (trailing underscore)
- **Constants / Enum values:** `SCREAMING_SNAKE_CASE`
- **Global variables (non-constexpr):** `GlobalPascalCase` (e.g. `GlobalLogger`)
- **Macros:** `SCREAMING_SNAKE_CASE` (e.g. `WFX_IS_TTY`)

**Example:**

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

### Before Opening a PR

**Formatting** is enforced via clang-format:

```bash
bash scripts/format.sh              # check everything (dry run, shows diffs)
bash scripts/format.sh --files path/to/file.cpp path/to/other.hpp  # check specific files only
bash scripts/format.sh --fix        # apply formatting in-place
```

Same convention as `tidy.sh` below: no flag means dry run, `--fix` is what actually writes to disk.

If you have sections with manual alignment or complex macro blocks that clang-format mangles, wrap them with `// clang-format off` and `// clang-format on`. Use it sparingly :)

**Static analysis** is done via clang-tidy (`sudo apt install clang-tidy`, or your distro's equivalent):

```bash
bash scripts/tidy.sh              # everything
bash scripts/tidy.sh --changed    # only what you changed vs main
bash scripts/tidy.sh --fix        # apply auto-fixes where possible
```

`tidy.sh` needs a compile database to run; it configures one itself in `build/` on first run if it's not there yet (no need to run `install.sh --local-debug` first). Files touching recent OpenSSL APIs won't fully resolve until something actually builds `build/` though - `install.sh --local-debug` (or `--local-release`) does, and it's the same directory, so running either once covers it.

---

### Pull Request Guidelines

- Fork the repo, create a branch, and open a PR to `dev`. DO NOT PR to `main`.
- Keep commits focused and meaningful (unless u wanna do some tomfoolery).
- Run `format.sh`, `tidy.sh`, and `tests/run_audits.sh` locally first.

---

### CI / .ciignore

- Files listed in `.ciignore` do **not** trigger CI if they are the only changes.
- Any other code changes trigger full CI.
- CI runs for PRs targeting `dev` or `main`.

---

So yeah, have fun contributing, ig.