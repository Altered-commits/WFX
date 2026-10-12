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
- **Git** (only needed for [Build from source](#build-from-source) below, not for a prebuilt install)
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

!!! warning "One install family per machine"
    A machine is locked to whichever family it first installed with: plain end-user/`--local-*`
    ("local"), or `--prebuilt-*` ("prebuilt"). Re-running `install.sh` under a different family
    is refused, run `bash ~/.wfx/src/scripts/uninstall.sh` first to switch. Re-running with a
    flag from the *same* family (e.g. `--prebuilt-known` again with a newer version) just
    updates your existing install.

---

## Install

The fastest way to get `wfx` running: download a prebuilt binary and headers from a GitHub
Release instead of building the engine yourself. A compiler and CMake are still needed on your
machine though: your own project's route-handler code always gets compiled locally into a shared
library that `wfx` `dlopen`s, prebuilt engine or not.

For a guided walkthrough, picking a version and CPU target interactively, WFX checks for that
compiler/CMake and offers to install whatever's missing:

```bash
curl -fsSL https://raw.githubusercontent.com/Altered-commits/WFX/main/scripts/install.sh | sh -s -- --prebuilt-interactive
```

For scripted/unattended use (provisioning scripts, EC2 user-data, Dockerfiles), no prompts and no
prerequisite checking either, this is assumed to already be part of your own provisioning:

```bash
curl -fsSL https://raw.githubusercontent.com/Altered-commits/WFX/main/scripts/install.sh \
    | sh -s -- --prebuilt-known <version> <target>
```

- `<version>` is a release tag (e.g. `v1.2.3`) or the literal word `latest`.
- `<target>` is one of:

    | Target | Description |
    |---|---|
    | `linux-x86_64-v2` | x86-64, safe baseline (any 2009+ Intel/AMD box) |
    | `linux-x86_64-v3` | x86-64, AVX2 (most current-gen cloud Intel/AMD) |
    | `linux-arm64-neoverse-n1` | ARM64, AWS Graviton2 (t4g family) |
    | `linux-arm64-neoverse-v1` | ARM64, AWS Graviton3 (c7g/m7g family) |

This creates the following structure under your home directory:

```
~/.wfx/
~/.wfx/bin/wfx                     # downloaded binary
~/.wfx/src/include/                # downloaded headers
~/.wfx/src/shared/                 # downloaded headers
~/.wfx/src/scripts/uninstall.sh    # downloaded, works the same as below
~/.wfx/daemons/                    # PID files for running servers (managed by wfx itself)
```

It also appends the following line to your shell config (`~/.bashrc` on Linux):

```bash
export PATH="$HOME/.wfx/bin:$PATH"
```

That is all it touches. No system directories, no sudo required after dependencies are installed.

After the script finishes, restart your terminal or run `source ~/.bashrc`, then verify with
`wfx`. You should see **WFX** printed.

---

## Build from source

If you'd rather build the engine itself locally, e.g. no prebuilt target matches your CPU, or
you just prefer not to run a downloaded binary, `install.sh` with no flags clones the full repo
and builds it:

```bash
curl -fsSL https://raw.githubusercontent.com/Altered-commits/WFX/main/scripts/install.sh | sh
```

If you need the latest (potentially unstable) changes from the `dev` branch:

```bash
curl -fsSL https://raw.githubusercontent.com/Altered-commits/WFX/dev/scripts/install.sh | sh
```

The `dev` branch gets new features and fixes first but may be less stable than `main`. Use it only if you specifically need something not yet in `main`.

After the script finishes, restart your terminal or run `source ~/.bashrc`, then verify with
`wfx`. You should see **WFX** printed.

This creates the following structure under your home directory:

```
~/.wfx/
~/.wfx/bin/            # wfx binary lives here
~/.wfx/src/            # cloned repository, built in ~/.wfx/src/build_install
~/.wfx/daemons/        # PID files for running servers (managed by wfx itself)
```

Same PATH setup as the prebuilt install above. That is all it touches, no system directories, no
sudo required after dependencies are installed.

!!! tip "Contributing to WFX?"
    This isn't the contributor path, it clones a fresh, disposable copy for your own project to
    build against. See [Development Setup](../dev_reference/dev_setup.md) in the Developer
    Reference instead: forking, `--local-*` flags, formatting, and testing all live there.

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
