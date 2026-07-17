#!/usr/bin/env bash
set -e

# ---------------------------------------------------------------
# WFX Installer
# Supports: Linux only
# Usage: curl -fsSL https://raw.githubusercontent.com/.../install.sh | sh
#
# Three distinct modes, same last step: CMake always drops the built 'wfx'-
# -binary at the source root, and this script moves it into ~/.wfx/bin/wfx.
#
#   End-user (no flag, curl-style): ~/.wfx/src is a real clone, built in-
#   -~/.wfx/src/build_install as a plain optimized Release build, sanitizers-
#   -off. Nothing here depends on any local checkout; this is the "just-
#   -install and use it" path.
#
#   --local-debug (contributor/dev mode): run from inside a git checkout of-
#   -this repo. ~/.wfx/src is a SYMLINK to that checkout (never a copy) so-
#   -nothing is duplicated, built directly in the checkout's own ./build as-
#   -a Debug build (full symbols, no optimization) with ASan+UBSan on, so-
#   -memory bugs surface immediately during normal dev use instead of only-
#   -in CI. Noticeably slower than the other two modes, that's expected.
#
#   --local-release (contributor/perf-testing mode): same checkout symlink-
#   -and ./build directory as --local-debug, but configured as an optimized-
#   -Release build with sanitizers off - identical settings to the end-user-
#   -build, just from your own checkout. Switching between this and-
#   ---local-debug reconfigures and recompiles whatever the flag change-
#   -touches, same as changing any other CMake option.
#
#   Re-run whichever of the two --local-* flags you last used after-
#   -pulling/editing to refresh the PATH binary, or just run ./wfx directly-
#   -from the checkout root while iterating.
#
# A machine is locked to whichever mode it first installed with (recorded-
# -in ~/.wfx/.install_type) - re-running install.sh with a different mode-
# -family (end-user vs local) is refused. --local-debug and --local-release-
# -both count as "local" and can be freely re-run interchangeably. Run-
# -scripts/uninstall.sh first to switch between end-user and local.
#
# Final folder structure (end-user mode):
#   ~/.wfx/
#   ~/.wfx/bin/
#   ~/.wfx/bin/wfx
#   ~/.wfx/src/                 (cloned repo)
#   ~/.wfx/src/build_install/   (cmake build artifacts)
#   ~/.wfx/daemons/
# ---------------------------------------------------------------

WFX_HOME="$HOME/.wfx"
WFX_BIN="$WFX_HOME/bin"
WFX_SRC="$WFX_HOME/src"
WFX_REPO="https://github.com/Altered-commits/WFX.git"
WFX_BINARY="$WFX_BIN/wfx"
WFX_INSTALL_TYPE_FILE="$WFX_HOME/.install_type"

# Absolute path to this checkout's root (scripts/install.sh -> repo root),-
# -needed so the local-mode symlinks stay valid regardless of how this script-
# -was invoked (relative path, symlinked into PATH, etc.)
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ---------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------
info()    { printf "\033[1;34m[WFX]\033[0m %s\n" "$*"; }
success() { printf "\033[1;32m[WFX]\033[0m %s\n" "$*"; }
warn()    { printf "\033[1;33m[WFX]\033[0m %s\n" "$*"; }
error()   { printf "\033[1;31m[WFX]\033[0m %s\n" "$*" >&2; exit 1; }

# ---------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------
# LOCAL_MODE is "" (end-user), "debug", or "release"
LOCAL_MODE=""
for arg in "$@"; do
    case "$arg" in
        --local-debug)   LOCAL_MODE="debug" ;;
        --local-release) LOCAL_MODE="release" ;;
        *) error "Unknown argument: $arg" ;;
    esac
done

if [ -n "$LOCAL_MODE" ]; then
    REQUESTED_TYPE="local"
else
    REQUESTED_TYPE="end-user"
fi

# ---------------------------------------------------------------
# Enforce a single install type per machine
# ---------------------------------------------------------------
# Once a machine has installed WFX as either local (--local-debug/--local-release)-
# -or end-user, it stays that type until 'uninstall.sh' wipes $WFX_HOME (which-
# -removes this marker along with everything else). Mixing the two on the same-
# -$WFX_HOME is what causes $WFX_SRC to flip between a symlink and a real clone-
# -from under whichever mode isn't currently active - not supported, so refuse-
# -instead of silently clobbering the other mode's state
if [ -f "$WFX_INSTALL_TYPE_FILE" ]; then
    EXISTING_TYPE="$(cat "$WFX_INSTALL_TYPE_FILE")"
    if [ "$EXISTING_TYPE" != "$REQUESTED_TYPE" ]; then
        error "WFX is already installed here as '$EXISTING_TYPE' (run 'install.sh $([ "$EXISTING_TYPE" = "local" ] && echo "--local-debug|--local-release")' to update it, or run scripts/uninstall.sh first to switch modes)."
    fi
fi

# ---------------------------------------------------------------
# Detect OS
# ---------------------------------------------------------------
detect_os() {
    case "$(uname -s)" in
        Linux*) echo "linux" ;;
        *)      error "Unsupported OS: $(uname -s) (WFX currently supports Linux only)" ;;
    esac
}

OS=$(detect_os)
info "Detected OS: $OS"

# ---------------------------------------------------------------
# Check dependencies
# ---------------------------------------------------------------
check_dep() {
    if ! command -v "$1" > /dev/null 2>&1; then
        error "Required dependency '$1' not found. $2"
    fi
}

info "Checking dependencies..."

check_dep git   "Install git and try again."
check_dep cmake "Install cmake (apt install cmake) and try again."

# Detect generator: prefer Ninja, fall back to make
if command -v ninja > /dev/null 2>&1; then
    GENERATOR="Ninja"
    BUILD_TOOL="ninja"
else
    warn "Ninja not found, falling back to Unix Makefiles"
    check_dep make "Install make and try again."
    GENERATOR="Unix Makefiles"
    BUILD_TOOL="make"
fi

# ---------------------------------------------------------------
# Create directory structure
# ---------------------------------------------------------------
info "Setting up ~/.wfx directory structure..."
mkdir -p "$WFX_BIN"
mkdir -p "$WFX_HOME/daemons"

# ---------------------------------------------------------------
# Clone or update source
# ---------------------------------------------------------------
if [ -n "$LOCAL_MODE" ]; then
    info "Dev mode: linking $WFX_SRC -> $REPO_ROOT..."
    # Clear out whatever end-user (or older --local) state might already be-
    # -there - a plain real directory has nothing worth preserving (it's-
    # -entirely install-generated), and 'ln -sfn' can't replace one on its own
    if [ -d "$WFX_SRC" ] && [ ! -L "$WFX_SRC" ]; then
        rm -rf "$WFX_SRC"
    fi
    ln -sfn "$REPO_ROOT" "$WFX_SRC" || error "Failed to link $WFX_SRC to $REPO_ROOT."
else
    # Clear out a stale local-mode symlink before treating $WFX_SRC as a real-
    # -clone target below
    if [ -L "$WFX_SRC" ]; then
        rm -f "$WFX_SRC"
    fi
    mkdir -p "$WFX_SRC"

    if [ -d "$WFX_SRC/.git" ]; then
        info "Source already exists, updating..."
        git -C "$WFX_SRC" pull --ff-only || error "Failed to update WFX source. Resolve conflicts in $WFX_SRC manually."
    else
        info "Cloning WFX repository..."
        git clone --depth=1 "$WFX_REPO" "$WFX_SRC" || error "Failed to clone WFX repository."
    fi
fi

# ---------------------------------------------------------------
# Build
# ---------------------------------------------------------------
info "Configuring build..."
if [ -n "$LOCAL_MODE" ]; then
    # Configure against the real checkout, not the $WFX_SRC symlink. CMake bakes-
    # -whatever '-S' it's given verbatim into compile_commands.json's file paths,-
    # -so configuring via the symlink poisons it for any tool (clang-tidy) that-
    # -does exact-string path matching against the real checkout path
    SOURCE_DIR="$REPO_ROOT"
    # Same build dir for both local modes - switching between them just makes-
    # -CMake reconfigure and Ninja recompile whatever the flag change touches
    BUILD_DIR="$REPO_ROOT/build"

    if [ "$LOCAL_MODE" = "release" ]; then
        # Same settings as the end-user build below, just from this checkout
        BUILD_TYPE="Release"
        ASAN_FLAG="-DWFX_ENABLE_ASAN=OFF"
    else
        BUILD_TYPE="Debug"
        ASAN_FLAG="-DWFX_ENABLE_ASAN=ON"
    fi
else
    BUILD_DIR="$WFX_SRC/build_install"
    SOURCE_DIR="$WFX_SRC"
    BUILD_TYPE="Release"
    ASAN_FLAG="-DWFX_ENABLE_ASAN=OFF"
fi

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
    -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
    "$ASAN_FLAG" \
    || error "CMake configuration failed."

info "Building WFX (this may take a moment)..."
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" \
    || error "Build failed."

# ---------------------------------------------------------------
# Install binary
# ---------------------------------------------------------------
info "Installing binary to $WFX_BINARY..."
# CMake always drops the built binary at the source root ($WFX_SRC/wfx),-
# -same for both modes. $WFX_BIN itself is never a symlink, only $WFX_SRC-
# -is (in local mode) - remove whatever's currently at $WFX_BINARY first-
# -since 'mv' onto an existing symlink would write through it instead of-
# -replacing the link itself
rm -f "$WFX_BINARY"
mv "$WFX_SRC/wfx" "$WFX_BINARY" || error "Failed to move binary to $WFX_BIN."
chmod +x "$WFX_BINARY"

# Record the install type so future runs on this machine are locked to it-
# -(see the enforcement check above)
echo "$REQUESTED_TYPE" > "$WFX_INSTALL_TYPE_FILE"

# ---------------------------------------------------------------
# Add to PATH
# ---------------------------------------------------------------
PATH_LINE='export PATH="$HOME/.wfx/bin:$PATH"'
PATH_JUST_ADDED=0

add_to_path() {
    local file="$1"
    if [ -f "$file" ]; then
        if grep -qF '.wfx/bin' "$file"; then
            return
        fi
        printf '\n# WFX\n%s\n' "$PATH_LINE" >> "$file"
        info "Added WFX to PATH in $file"
        PATH_JUST_ADDED=1
    fi
}

add_to_path "$HOME/.bashrc"
add_to_path "$HOME/.profile"

# ---------------------------------------------------------------
# GG
# ---------------------------------------------------------------
success "WFX installed successfully!"
success "Details:"
success "    Root   -> $WFX_HOME"
success "    Binary -> $WFX_BINARY"
echo ""

if [ "$PATH_JUST_ADDED" = "1" ]; then
    info "Restart your terminal or run this once:"
    printf "    source ~/.bashrc\n\n"
fi

info "Then try:"
printf "    wfx new my-project\n\n"