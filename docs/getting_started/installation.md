# Installation

!!! important
    WFX currently supports **Linux only**.

    - Linux: Supported
    - WSL: Supported
    - Windows (native): Not supported
    - macOS: Not supported

---

## Overview

This section covers installing WFX, what it puts on your system, and how to remove it cleanly.

### Prerequisites

- **C++20 compiler** (GCC or Clang)
- **CMake 3.20+**
- **Git**
- **Ninja** (optional, recommended for faster builds)

Install required tools on common Linux distributions:

- Ubuntu / Debian
    ```bash
    sudo apt update
    sudo apt install -y build-essential cmake git

    # Optional (recommended)
    sudo apt install -y ninja-build
    ```

- Fedora
    ```bash
    sudo dnf install -y gcc-c++ cmake git

    # Optional (recommended)
    sudo dnf install -y ninja-build
    ```

- Arch Linux
    ```bash
    sudo pacman -S --needed base-devel cmake git

    # Optional (recommended)
    sudo pacman -S ninja
    ```

---

## Install via Script (Recommended)

The install script handles everything: cloning, building, and adding `wfx` to your PATH.

```bash
curl -fsSL https://raw.githubusercontent.com/Altered-commits/WFX/main/scripts/install.sh | sh
```

If you need the latest (potentially unstable) changes from the `dev` branch:

```bash
curl -fsSL https://raw.githubusercontent.com/Altered-commits/WFX/dev/scripts/install.sh | sh
```

The `dev` branch gets new features and fixes first but may be less stable than `main`. Use it only if you specifically need something not yet in `main`.

After the script finishes, restart your terminal or run:

```bash
source ~/.bashrc
```

Then verify:

```bash
wfx
```

You should see **WFX** printed.

### What the installer does

For a plain end-user install (no flags), the installer creates the following structure under
your home directory:

```
~/.wfx/
~/.wfx/bin/         # wfx binary lives here
~/.wfx/src/         # cloned repository, built in ~/.wfx/src/build_install
~/.wfx/daemons/     # PID files for running servers (managed by wfx itself)
```

See [Local install](#local-install) below for how `--local-debug`/`--local-release`
(contributor mode) differ.

It also appends the following line to your shell config (`~/.bashrc` on Linux):

```bash
export PATH="$HOME/.wfx/bin:$PATH"
```

That is all it touches. No system directories, no sudo required after dependencies are installed.

### Local install

If you already have the source code cloned and want to build from it directly (the path
contributors use), there are two local modes, both making `~/.wfx/src` a **symlink** to your
checkout and building directly into its own `build/` directory:

```bash
# Dev mode: Debug build (full symbols, no optimization), ASan + UBSan on
# This is what you want day to day, bugs surface immediately
# Noticeably slower than a real install
bash scripts/install.sh --local-debug

# Perf-testing mode: same optimized, sanitizer-free settings as an-
# -end-user install, just built from your own checkout
bash scripts/install.sh --local-release
```

Either way the resulting binary is moved to `~/.wfx/bin/wfx`, same as the end-user path. Re-run
whichever of the two you last used after pulling/editing to refresh the PATH binary, or just run
`./wfx` directly from the checkout root while iterating. Switching between the two modes
reconfigures and recompiles whatever the flag change touches, they share the same `build/`
directory.

---

## Manual Install

If you prefer not to use the script:

**1. Create the directory structure**

```bash
mkdir -p ~/.wfx/bin ~/.wfx/src ~/.wfx/daemons
```

**2. Clone the repository**

```bash
git clone https://github.com/Altered-commits/WFX.git ~/.wfx/src
```

**3. Build**

```bash
# With Ninja (recommended)
cmake -S ~/.wfx/src -B ~/.wfx/src/build_install -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ~/.wfx/src/build_install

# Without Ninja
cmake -S ~/.wfx/src -B ~/.wfx/src/build_install -DCMAKE_BUILD_TYPE=Release
cmake --build ~/.wfx/src/build_install
```

**4. Copy the binary**

```bash
cp ~/.wfx/src/wfx ~/.wfx/bin/wfx
chmod +x ~/.wfx/bin/wfx
```

**5. Add to PATH**

Add the following line to your `~/.bashrc` or `~/.profile`:

```bash
export PATH="$HOME/.wfx/bin:$PATH"
```

Then reload:

```bash
source ~/.bashrc
```

---

## Uninstall

Run the uninstall script:

```bash
bash ~/.wfx/src/scripts/uninstall.sh
```

The uninstaller will:

1. Check for running WFX daemons and warn you if any are active
2. Remove the `export PATH` line from your shell config files
3. Delete `~/.wfx` entirely

If you have running servers, stop them first:

```bash
wfx control stop <project>
```

Or force uninstall anyway when prompted.

After uninstalling, restart your terminal to apply the PATH changes.

---

## Notes

- Windows and macOS support is planned for a future release.
- The most recent code is on the `dev` branch. The `main` branch will host stable releases once the project matures. Until then, `dev` is the recommended branch.
- WFX is currently tested on Ubuntu and Debian-based distributions. Other Linux distributions should work but are untested.
- Kernel TLS (used for HTTPS acceleration) requires a recent Linux kernel. Older kernels will compile but may not support all runtime features.

---

Continue to **[Your First WFX Program](first_program.md)**