#!/usr/bin/env bash
set -e

# ---------------------------------------------------------------
# WFX Installer
# Supports: Linux, macOS
# Usage: curl -fsSL https://raw.githubusercontent.com/.../install.sh | sh
#
# Final folder structure:
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
LOCAL=0
for arg in "$@"; do
    case "$arg" in
        --local) LOCAL=1 ;;
        *) error "Unknown argument: $arg" ;;
    esac
done

# ---------------------------------------------------------------
# Detect OS
# ---------------------------------------------------------------
detect_os() {
    case "$(uname -s)" in
        Linux*)  echo "linux"  ;;
        Darwin*) echo "macos"  ;;
        *)       error "Unsupported OS: $(uname -s)" ;;
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
check_dep cmake "Install cmake (apt install cmake / brew install cmake) and try again."

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

# OpenSSL check
if [ "$OS" = "linux" ]; then
    if ! dpkg -s libssl-dev > /dev/null 2>&1 && ! pkg-config --exists openssl 2>/dev/null; then
        error "OpenSSL development headers not found. Run: sudo apt install libssl-dev"
    fi
elif [ "$OS" = "macos" ]; then
    if ! brew list openssl > /dev/null 2>&1; then
        error "OpenSSL not found. Run: brew install openssl"
    fi
fi

# ---------------------------------------------------------------
# Create directory structure
# ---------------------------------------------------------------
info "Setting up ~/.wfx directory structure..."
mkdir -p "$WFX_BIN"
mkdir -p "$WFX_SRC"
mkdir -p "$WFX_HOME/daemons"

# ---------------------------------------------------------------
# Clone or update source
# ---------------------------------------------------------------
if [ "$LOCAL" = "1" ]; then
    info "Local mode: copying current directory to $WFX_SRC..."
    rsync -a --exclude='.git' --exclude='build' --exclude='.venv' . "$WFX_SRC/" \
        || error "Failed to copy source to $WFX_SRC."
else
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
BUILD_DIR="$WFX_SRC/build_install"

cmake -S "$WFX_SRC" -B "$BUILD_DIR" \
    -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    || error "CMake configuration failed."

info "Building WFX (this may take a moment)..."
cmake --build "$BUILD_DIR" --config Release \
    || error "Build failed."

# ---------------------------------------------------------------
# Install binary
# ---------------------------------------------------------------
info "Installing binary to $WFX_BINARY..."
cp "$WFX_SRC/wfx" "$WFX_BINARY" || error "Failed to copy binary to $WFX_BIN."
chmod +x "$WFX_BINARY"

# ---------------------------------------------------------------
# Add to PATH
# ---------------------------------------------------------------
PATH_LINE='export PATH="$HOME/.wfx/bin:$PATH"'

add_to_path() {
    local file="$1"
    if [ -f "$file" ]; then
        if grep -qF '.wfx/bin' "$file"; then
            return
        fi
        printf '\n# WFX\n%s\n' "$PATH_LINE" >> "$file"
        info "Added WFX to PATH in $file"
    fi
}

if [ "$OS" = "macos" ]; then
    add_to_path "$HOME/.zshrc"
    add_to_path "$HOME/.bash_profile"
else
    add_to_path "$HOME/.bashrc"
    add_to_path "$HOME/.profile"
fi

# ---------------------------------------------------------------
# GG
# ---------------------------------------------------------------
success "WFX installed successfully!"
success "Details:"
success "    Root   -> $WFX_HOME"
success "    Binary -> $WFX_BINARY"
echo ""
info "Restart your terminal or run:"

if [ "$OS" = "macos" ]; then
    printf "    source ~/.zshrc\n\n"
else
    printf "    source ~/.bashrc\n\n"
fi

info "Then try:"
printf "    wfx new my-project\n\n"