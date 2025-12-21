# WFX

!!! warning "Note"
    The documentation and APIs are under active development. They may change frequently, so do not expect this documentation to remain fully accurate over time.

---

**Explicit, low-level C++ web engine and a framework** for people who want **control** and **performance** without hiding behavior behind too many abstractions.

It provides:

- A full HTTP(S) server engine
- A deterministic request-response lifecycle
- Global and per-route middleware
- Streaming (outbound for now)
- Connection timeouts, rate limiting and security primitives
- A server-side rendering (SSR) engine with templating
- Custom asynchronous execution model
- Built-in form parsing, validation, sanitization and field rendering
- TOML based configuration

Its design principles are:

- **Engine-as-source, not a black box**  
  WFX is not a closed runtime. The framework headers and engine code
  are part of your build, not an opaque dependency.

- **Minimal magic by default**  
  Behavior is explicit unless you opt into helper macros
  (for example, coroutine helpers instead of manual state machines).

- **Deterministic execution**  
  Request handling, middleware order, and ownership semantics
  are predictable and controllable.

- **Clear separation of responsibilities**  
  The engine, framework features, and user code are distinct layers,
  even though they are compiled together.

---

## Who this is for

WFX is a good fit if you:

- Are comfortable with C++
- Care about performance and memory behavior without sacrificing built-in features
- Want full control over engine and framework behavior
- Prefer transparent behavior over hidden abstractions

WFX is **not** for you if you:

- Expect instant hot reload or scripting-language iteration speeds (compiling engine + user code takes time)
- Just want to serve simple static pages or single-page apps without managing server-side logic
- Are uncomfortable with pointers or ownership semantics
- Hate C++

---

## Documentation structure

If you are new, start here:

-> Continue to **[Installation](getting_started/installation.md)**

If you already know what you are doing:

-> Continue to **[API documentation](api_reference/overview.md)**

---

## Developer note

Well if you are considering to use my framework - first of all, Thank You!  
Right now, i wouldn't recommend you use it for real software, as i will be making a lot of breaking change (and fixing a lot of bugs).  
You can use it to help contribute to the framework ofc.  
Other than that, best of luck. You need it :)

---

Continue to **[Getting Started](getting_started/installation.md)**