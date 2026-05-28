# Contributing to WFX

I'm surprised you actually decided to contribute to this 'whatever' of a project. Thank you 'user'.  
So some guidelines and stuff to get you an idea (I don't even remember half the stuff but yeah).  

Note: See the [README](https://github.com/Altered-commits/WFX/new/altered/dev#build) for build instructions.  

Fork the repo and open PRs to `dev`. `main` and `dev` are protected. Work freely in your fork or personal branch.

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
- **Variables / Function parameters / Locals:** `camelCase`
- **Member variables:** `camelCase_` (trailing underscore)
- **Constants / Enum values:** `SCREAMING_SNAKE_CASE`
- **Globals:** `__PascalCase` (e.g. `__GlobalLogger`)
- **Macros:** `SCREAMING_SNAKE_CASE` (e.g. `WFX_IS_TTY`)

**Formatting** is enforced via clang-format. Before opening a PR, run:

```bash
bash scripts/format.sh
```

To format specific files only:

```bash
bash scripts/format.sh --files path/to/file.cpp path/to/other.hpp
```

If you have sections with manual alignment or complex macro blocks that clang-format mangles, wrap them with `// clang-format off` and `// clang-format on`. Use it sparingly :)

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

### Pull Request Guidelines

- Fork the repo, create a branch, and open a PR to `dev`. DO NOT PR to `main`.
- Keep commits focused and meaningful (unless u wanna do some tomfoolery).
- Run CI locally if possible, cuz i'm poor and i don't have too many CI minutes :(.

---

### CI / .ciignore

- Files listed in `.ciignore` do **not** trigger CI if they are the only changes.
- Any other code changes trigger full CI.
- CI runs for PRs targeting `dev` or `main`.

---

So yeah, have fun contributing, ig.