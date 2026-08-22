# Development Setup

This page covers getting a WFX checkout building, formatted, tested, and the docs site previewing
locally. If you just want to run WFX as an end user, use [Installation](../getting_started/installation.md)
instead, none of this page applies to that path.

---

## Clone and build

Fork the repo, then clone your fork checked out to `dev`. This needs the same compiler/CMake/Git
(and optionally Ninja) prerequisites as the end-user install, see
[Installation's Prerequisites](../getting_started/installation.md#prerequisites) if you don't have
those yet.

```bash
git clone -b dev https://github.com/<you>/WFX.git
cd WFX
bash scripts/install.sh --local-debug
```

`main` and `dev` are protected. Work in your fork or a personal branch and open PRs against `dev`,
never `main`.

`--local-debug` symlinks `~/.wfx/src` back to your checkout (nothing duplicated) and builds
straight into the checkout's own `build/`, Debug, full symbols, AddressSanitizer +
UndefinedBehaviorSanitizer on, so dangling-pointer/use-after-free/UB bugs surface as soon as you
hit them locally instead of only in CI. That makes the resulting binary noticeably slower than a
real install. If you need a fast build for perf testing, use `bash scripts/install.sh
--local-release` instead: same checkout, same `build/` directory, optimized Release with
sanitizers off.

After editing, either run `./wfx` from the checkout root while iterating, or rebuild with
whichever `--local-*` mode you last used to refresh `wfx` on PATH. Switching between the two modes
reconfigures and recompiles whatever the flag change touches, they share the same `build/`
directory. See [Build Macros](build_macros.md) for exactly what each mode sets under the hood.

---

## Formatting

Enforced via clang-format. Install it first:

- Ubuntu / Debian
    ```bash
    sudo apt update
    sudo apt install -y clang-format
    ```

- Fedora
    ```bash
    sudo dnf install -y clang-tools-extra
    ```
    This package also provides clang-tidy, needed for the next section, one install covers both.

- Arch Linux
    ```bash
    sudo pacman -S --needed clang
    ```
    Arch's `clang` package ships both clang-format and clang-tidy.

Then:

```bash
bash scripts/format.sh              # check everything (dry run, shows diffs)
bash scripts/format.sh --files path/to/file.cpp path/to/other.hpp  # check specific files only
bash scripts/format.sh --fix        # apply formatting in-place
```

No flag means dry run, `--fix` is what actually writes to disk, same convention as `tidy.sh`
below. If a section has manual alignment or a macro block clang-format mangles, wrap it with
`// clang-format off` / `// clang-format on`, sparingly.

---

## Static analysis

Done via clang-tidy:

- Ubuntu / Debian
    ```bash
    sudo apt update
    sudo apt install -y clang-tidy
    ```

- Fedora
    ```bash
    sudo dnf install -y clang-tools-extra
    ```
    Same package as clang-format above, skip this if you already installed it there.

- Arch Linux
    ```bash
    sudo pacman -S --needed clang
    ```
    Same package as clang-format above, skip this if you already installed it there.

Then:

```bash
bash scripts/tidy.sh              # everything
bash scripts/tidy.sh --changed    # only what you changed vs main
bash scripts/tidy.sh --fix        # apply auto-fixes where possible
```

`tidy.sh` needs a compile database to run; it configures one itself in `build/` on first run if
one isn't there yet, no need to run `install.sh --local-debug` first for that part. Files touching
recent OpenSSL APIs won't fully resolve until something actually builds `build/` though,
`install.sh --local-debug` (or `--local-release`) does, and it's the same directory, so running
either once covers it.

---

## Testing

The audits are plain Python scripts, standard library only, no pip packages needed. Install
Python 3.8+ if you don't already have it:

- Ubuntu / Debian
    ```bash
    sudo apt update
    sudo apt install -y python3
    ```

- Fedora
    ```bash
    sudo dnf install -y python3
    ```

- Arch Linux
    ```bash
    sudo pacman -S --needed python
    ```

See [Testing](testing.md) for what each audit covers and how to run one. Before opening a PR, run
`tests/run_audits.sh` locally, it covers six of the seven audits.

The seventh, `interop_audit`, additionally needs Docker with the `docker compose` plugin (the
plugin, not the standalone `docker-compose` binary):

- Ubuntu / Debian
    ```bash
    sudo apt update
    sudo apt install -y docker.io docker-compose-v2
    # if apt can't find those two packages, enable the universe repo first:
    #   sudo add-apt-repository universe
    ```

- Arch Linux
    ```bash
    sudo pacman -S --needed docker docker-compose
    ```

- Fedora
    Fedora's own repos don't reliably ship the `docker compose` plugin form, so this one needs
    Docker's official repo. Follow
    [Docker's Fedora install guide](https://docs.docker.com/engine/install/fedora/), the short
    version:
    ```bash
    sudo dnf -y install dnf-plugins-core
    sudo dnf config-manager --add-repo https://download.docker.com/linux/fedora/docker-ce.repo
    sudo dnf install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
    ```

On all three, start the daemon and add yourself to the `docker` group so you don't need `sudo` for
every `docker compose` call (log out and back in, or `newgrp docker`, for the group change to
take effect):

```bash
sudo systemctl enable --now docker
sudo usermod -aG docker "$USER"
```

`interop_audit` is left out of the plain `tests/run_audits.sh` run (not every dev machine has
Docker running) and reached separately: `tests/run_audits.sh --audit interop`. See
`tests/interop_audit/README.md` for what it drives.

---

## Docs

Docs are built with [MkDocs Material](https://squidfunk.github.io/mkdocs-material/) from `docs/`
and `mkdocs.yml` at the repo root. Install Python (same as Testing above, skip if you already have
it), create a virtual environment, and install MkDocs Material into it:

- Ubuntu / Debian
    ```bash
    sudo apt update
    sudo apt install -y python3 python3-venv
    python3 -m venv .venv
    source .venv/bin/activate
    pip install mkdocs-material
    ```

- Fedora
    ```bash
    sudo dnf install -y python3
    python3 -m venv .venv
    source .venv/bin/activate
    pip install mkdocs-material
    ```

- Arch Linux
    ```bash
    sudo pacman -S --needed python
    python3 -m venv .venv
    source .venv/bin/activate
    pip install mkdocs-material
    ```

`.venv/` is already gitignored. Once it's activated (your prompt gets a `(.venv)` prefix), preview
the site:

```bash
mkdocs serve
```

That serves the site at `http://127.0.0.1:8000` with live reload. Leave the venv with `deactivate`
when you're done; re-activate it with `source .venv/bin/activate` next time instead of
reinstalling.

You don't need to deploy anything yourself: `.github/workflows/docs_build.yml` runs `mkdocs
gh-deploy` on every push to `main` and publishes the result to GitHub Pages.

---

## Before Opening a PR

- Fork the repo, create a branch, and open a PR to `dev`. DO NOT PR to `main`.
- Keep commits focused and meaningful (unless u wanna do some tomfoolery).
- Run `format.sh`, `tidy.sh`, and `tests/run_audits.sh` locally first.

### CI / .ciignore

- Files listed in `.ciignore` do **not** trigger CI if they are the only changes.
- Any other code changes trigger full CI.
- CI runs for PRs targeting `dev` or `main`.

See [Coding Conventions](codebase_architecture.md#naming-conventions) for naming rules.
