#!/usr/bin/env bash
set -e

# ---------------------------------------------------------------
# WFX Uninstaller
# Supports: Linux only
# ---------------------------------------------------------------

WFX_HOME="$HOME/.wfx"
WFX_BIN="$WFX_HOME/bin"

# ---------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------
info()    { printf "\033[1;34m[WFX]\033[0m %s\n" "$*"; }
success() { printf "\033[1;32m[WFX]\033[0m %s\n" "$*"; }
warn()    { printf "\033[1;33m[WFX]\033[0m %s\n" "$*"; }
error()   { printf "\033[1;31m[WFX]\033[0m %s\n" "$*" >&2; exit 1; }

# ---------------------------------------------------------------
# Detect OS
# ---------------------------------------------------------------
case "$(uname -s)" in
    Linux*) ;;
    *)      error "Unsupported OS: $(uname -s) (WFX currently supports Linux only)" ;;
esac

# ---------------------------------------------------------------
# Check for running daemons
# ---------------------------------------------------------------
DAEMONS_DIR="$WFX_HOME/daemons"
if [ -d "$DAEMONS_DIR" ] && [ -n "$(ls -A "$DAEMONS_DIR" 2>/dev/null)" ]; then
    warn "There are active WFX daemon PID files in $DAEMONS_DIR."
    warn "Stop all running servers with 'wfx control stop <project>' before uninstalling."
    printf "\nContinue anyway? [y/N] "
    read -r answer
    case "$answer" in
        [yY]) ;;
        *) info "Uninstall cancelled."; exit 0 ;;
    esac
fi

# ---------------------------------------------------------------
# Remove PATH entry from shell configs
# ---------------------------------------------------------------
remove_from_path() {
    local file="$1"
    if [ -f "$file" ] && grep -qF '.wfx/bin' "$file"; then
        # Remove the WFX block (comment + export line)
        sed -i '/# WFX/d' "$file"
        sed -i '/\.wfx\/bin/d' "$file"
        info "Removed WFX from PATH in $file"
    fi
}

info "Removing WFX from PATH..."
remove_from_path "$HOME/.bashrc"
remove_from_path "$HOME/.profile"

# ---------------------------------------------------------------
# Remove ~/.wfx
# ---------------------------------------------------------------
info "Removing $WFX_HOME..."
rm -rf "$WFX_HOME" || error "Failed to remove $WFX_HOME. Try: rm -rf $WFX_HOME"

# ---------------------------------------------------------------
# Done
# ---------------------------------------------------------------
success "WFX uninstalled successfully."
info "Restart your terminal to apply PATH changes."