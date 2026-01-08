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
- `WFX::Utils` (not a direct part of engine code)

User code doesn't need global namespaces unless you wish to create them (like what i did for Async::)

---

### Coding Conventions

- **Namespaces / Classes / Structs / Enums / Function identifier:** `PascalCase`
- **Variables / Function parameters / Locals:** `camelCase`
- **Globals / Constants / Enum values:** `UPPER_SNAKE_CASE`
- **Internal engine-facing symbols:** prefix `__` (this is a bit debatable for now, use sparingly)

**Formatting:** 4 spaces, no tabs. Braces: same line for everything except function definitions (unless u wish to write entire function in a single line).

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

}  // namespace WFX::Utils
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