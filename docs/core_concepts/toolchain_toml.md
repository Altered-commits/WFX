# Toolchain Settings

This page defines **all required `toolchain.toml` compiler configuration options** used by WFX to build your source code.

!!! note
    All examples shown in code boxes use GNU G++ build settings. Values and flags may differ for other compilers.

!!! warning
    Do **not modify these settings** unless:  

    - You know exactly what you are doing, **or**  
    - WFX fails to auto-detect your compiler and you need to provide a custom setup.

    Modifying these values without understanding them can break builds or cause undefined behavior, as all settings are pre-tuned for compatibility with the WFX engine.

---

## `[Compiler]`

Global compiler configuration. **All settings are mandatory**.

<pre class="code-format">
[Compiler]
name    = "g++[gnu]"
ccmd    = "g++"
lcmd    = "g++"
objflag = "-o "
dllflag = "-o "
</pre>

- `name`: Human-readable identifier for the compiler. Used internally for logging, display, and tracking purposes; it does not affect the actual build process.
- `ccmd`: Command-line invocation for compiling `.cpp` files to `.o` objects.
- `lcmd`: Command-line invocation for linking object files into executables or libraries.
- `objflag`: Compiler flag to specify the output object file. Typically `-o`.
- `dllflag`: Compiler flag to specify the output shared library. Typically `-o`.

## `[Compiler.Prod]`

Compiler arguments for **production builds**.

<pre class="code-format">
[Compiler.Prod]
cargs = "-std=c++17 -fPIC -O3 -flto=auto -fno-plt -fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections -I. -IWFX/include -IWFX -c"
largs = "-shared -fPIC -flto=auto -Wl,--gc-sections -Wl,--strip-all"
</pre>

- `cargs`: Compiler flags for production: enables C++17, optimizations, link-time optimization, hidden visibility, and proper include paths. Produces `.o` files.
- `largs`: Linker flags for production: produces shared libraries, strips unused sections, and enables LTO for maximum performance.

## `[Compiler.Debug]`

Compiler arguments for **debug builds**.

<pre class="code-format">
[Compiler.Debug]
cargs = "-std=c++17 -fPIC -O0 -I. -IWFX/include -IWFX -c"
largs = "-shared -fPIC"
</pre>

- `cargs`: Compiler flags for debugging: disables optimizations, includes debug info, produces `.o` files.
- `largs`: Linker flags for debug: produces shared libraries without optimizations, keeping symbols readable for debugging.