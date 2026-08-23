#!/usr/bin/env bash
set -e

# ---------------------------------------------------------------
# WFX Installer
# Supports: Linux only
# Usage: curl -fsSL https://raw.githubusercontent.com/.../install.sh | sh
#
# Two families of mode, same last step either way: 'wfx' ends up at
# ~/.wfx/bin/wfx and ~/.wfx/src holds whatever a user project's CMakeLists.txt
# needs to build against (include/ and shared/, at minimum).
#
# --- Build from source ---
#
#   End-user (no flag, curl-style): ~/.wfx/src is a real clone, built in
#   ~/.wfx/src/build_install as a plain optimized Release build, sanitizers
#   off. Nothing here depends on any local checkout; this is the "just
#   install and use it" path.
#
#   --local-debug (contributor/dev mode): run from inside a git checkout of
#   this repo. ~/.wfx/src is a SYMLINK to that checkout (never a copy) so
#   nothing is duplicated, built directly in the checkout's own ./build as
#   a Debug build (full symbols, no optimization) with ASan+UBSan on, so
#   memory bugs surface immediately during normal dev use instead of only
#   in CI. Noticeably slower than the other modes, that's expected.
#
#   --local-release (contributor/perf-testing mode): same checkout symlink
#   and ./build directory as --local-debug, but configured as an optimized
#   Release build with sanitizers off - identical settings to the end-user
#   build, just from your own checkout. Switching between this and
#   --local-debug reconfigures and recompiles whatever the flag change
#   touches, same as changing any other CMake option.
#
#   Re-run whichever of the two --local-* flags you last used after
#   pulling/editing to refresh the PATH binary, or just run ./wfx directly
#   from the checkout root while iterating.
#
# --- Prebuilt (no compiling WFX itself) ---
#
# Downloads an already-built 'wfx' binary plus the include/shared headers
# from a GitHub Release instead of building the engine. Doesn't remove the
# need for a compiler/CMake/Ninja on this machine though: a user project's
# own route-handler code always gets compiled locally into a shared library
# WFX dlopens, prebuilt engine or not, see docs/dev_reference/architecture.md.
# Both variants check for (and, on confirmation, can install) those.
#
#   --prebuilt-interactive: fully interactive. Walks through picking a
#   version, then a CPU target, from whatever that release actually
#   published, nothing pre-selected for you, every step needs an explicit
#   confirm, including whether to install a missing compiler/CMake. See
#   run_prebuilt_install() below.
#
#   --prebuilt-known <version> <target>: the scripted twin, for anywhere a
#   human isn't watching (provisioning scripts, EC2 user-data, Dockerfiles).
#   No prompts, no prerequisite checking or suggestions either: whoever's
#   scripting this is assumed to already own their own toolchain setup.
#   <version> is a tag (e.g. v1.2.3) or the literal word 'latest'; <target>
#   is the name from release_build.yml's matrix without the 'wfx-' prefix,
#   e.g. linux-x86_64-v3 or linux-arm64-neoverse-n1.
#
# A machine is locked to whichever mode FAMILY it first installed with
# (recorded in ~/.wfx/.install_type) - re-running install.sh under a
# different family (end-user vs local vs prebuilt) is refused. Both
# --local-* flags count as "local" and both --prebuilt-* flags count as
# "prebuilt", freely re-runnable within their own family. Run
# scripts/uninstall.sh first to switch families.
#
# Final folder structure (end-user / local mode):
#   ~/.wfx/
#   ~/.wfx/bin/bin/wfx
#   ~/.wfx/src/                 (cloned repo, or a symlink to one in local mode)
#   ~/.wfx/src/build_install/   (cmake build artifacts, end-user mode only)
#   ~/.wfx/daemons/
#
# Final folder structure (prebuilt mode):
#   ~/.wfx/
#   ~/.wfx/bin/wfx                     (downloaded binary)
#   ~/.wfx/src/include/                (downloaded headers)
#   ~/.wfx/src/shared/                 (downloaded headers)
#   ~/.wfx/src/scripts/uninstall.sh    (downloaded, rides along with headers)
#   ~/.wfx/daemons/
# ---------------------------------------------------------------

WFX_HOME="$HOME/.wfx"
WFX_BIN="$WFX_HOME/bin"
WFX_SRC="$WFX_HOME/src"
WFX_REPO="https://github.com/Altered-commits/WFX.git"
WFX_GH_API="https://api.github.com/repos/Altered-commits/WFX"
WFX_BINARY="$WFX_BIN/wfx"
WFX_INSTALL_TYPE_FILE="$WFX_HOME/.install_type"

# Absolute path to this checkout's root (scripts/install.sh -> repo root),
# needed so the local-mode symlinks stay valid regardless of how this script
# was invoked (relative path, symlinked into PATH, etc.). Meaningless for
# --prebuilt-*, which never needs a checkout at all.
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ---------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------
info()    { printf "\033[1;34m[WFX]\033[0m %s\n" "$*" >&2; }
success() { printf "\033[1;32m[WFX]\033[0m %s\n" "$*" >&2; }
warn()    { printf "\033[1;33m[WFX]\033[0m %s\n" "$*" >&2; }
error()   { printf "\033[1;31m[WFX]\033[0m %s\n" "$*" >&2; }
fatal()   { error "$*"; exit 1; }

# ---------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------
# LOCAL_MODE: "" (end-user), "debug", or "release"
# PREBUILT_MODE: "" (not prebuilt), "interactive", or "known"
LOCAL_MODE=""
PREBUILT_MODE=""
PREBUILT_VERSION=""
PREBUILT_TARGET=""

while [ $# -gt 0 ]; do
    case "$1" in
        --local-debug)
            LOCAL_MODE="debug"
            shift
            ;;
        --local-release)
            LOCAL_MODE="release"
            shift
            ;;
        --prebuilt-interactive)
            PREBUILT_MODE="interactive"
            shift
            ;;
        --prebuilt-known)
            PREBUILT_MODE="known"
            shift
            if [ $# -lt 2 ]; then
                fatal "--prebuilt-known needs a version and a target, e.g. --prebuilt-known v1.2.3 linux-x86_64-v3"
            fi
            PREBUILT_VERSION="$1"
            PREBUILT_TARGET="$2"
            shift 2
            ;;
        *)
            fatal "Unknown argument: $1"
            ;;
    esac
done

if [ -n "$LOCAL_MODE" ]; then
    REQUESTED_TYPE="local"
elif [ -n "$PREBUILT_MODE" ]; then
    REQUESTED_TYPE="prebuilt"
else
    REQUESTED_TYPE="end-user"
fi

# ---------------------------------------------------------------
# Enforce a single install type per machine
# ---------------------------------------------------------------
# Once a machine has installed WFX as local, end-user, or prebuilt, it stays
# that family until 'uninstall.sh' wipes $WFX_HOME (which removes this
# marker along with everything else). Mixing families on the same $WFX_HOME
# is what causes $WFX_SRC to flip between a symlink, a real clone, and a
# headers-only directory from under whichever family isn't currently active
# - not supported, so refuse instead of silently clobbering the other
# family's state.
if [ -f "$WFX_INSTALL_TYPE_FILE" ]; then
    EXISTING_TYPE="$(cat "$WFX_INSTALL_TYPE_FILE")"
    if [ "$EXISTING_TYPE" != "$REQUESTED_TYPE" ]; then
        case "$EXISTING_TYPE" in
            local)    HINT="--local-debug|--local-release" ;;
            prebuilt) HINT="--prebuilt-interactive|--prebuilt-known" ;;
            *)        HINT="(no flags)" ;;
        esac
        fatal "WFX is already installed here as '$EXISTING_TYPE' (run 'install.sh $HINT' to update it, or run scripts/uninstall.sh first to switch modes)."
    fi
fi

# ---------------------------------------------------------------
# Detect OS
# ---------------------------------------------------------------
detect_os() {
    case "$(uname -s)" in
        Linux*) echo "linux" ;;
        *)      fatal "Unsupported OS: $(uname -s) (WFX currently supports Linux only)" ;;
    esac
}

OS=$(detect_os)
info "Detected OS: $OS"

# ---------------------------------------------------------------
# Shared tail: PATH setup + final message (used by every mode)
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

setup_path() {
    add_to_path "$HOME/.bashrc"
    add_to_path "$HOME/.profile"
}

print_success() {
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
}

# =================================================================
# Prebuilt install (--prebuilt-interactive / --prebuilt-known)
# =================================================================
if [ -n "$PREBUILT_MODE" ]; then

    # -------------------------------------------------------------
    # Interactive menu primitive
    # -------------------------------------------------------------
    # Arrow-key/Enter driven select menu. Reads and writes through /dev/tty
    # directly instead of stdin/stdout, so this still works when the script
    # itself arrived over a pipe (curl ... | sh -s -- --prebuilt-interactive),
    # the same trick rustup's installer uses. If there's no controlling
    # terminal at all, falls back to whichever option the caller passed
    # first instead of hanging on a read that can never receive input -
    # --prebuilt-known exists precisely so a fully non-interactive caller
    # never needs this fallback in the first place.
    #
    # Usage: interactive_select "Prompt text" "Label 1" "Label 2" ...
    # Result lands in $SELECT_RESULT (0-based index into the labels given).
    interactive_select() {
        local prompt="$1"
        shift
        local options=("$@")
        local count=${#options[@]}
        local selected=0

        if [ ! -r /dev/tty ] || [ ! -w /dev/tty ]; then
            warn "No interactive terminal available for: $prompt"
            warn "Defaulting to: ${options[0]}"
            SELECT_RESULT=0
            return
        fi

        local old_stty
        old_stty=$(stty -g < /dev/tty)
        # Restore terminal state no matter how we leave, Ctrl-C included
        trap 'stty "$old_stty" < /dev/tty 2>/dev/null; printf "\033[?25h" > /dev/tty; echo; exit 130' INT
        stty -echo -icanon min 1 time 0 < /dev/tty
        printf "\033[?25l" > /dev/tty # hide cursor

        local first_draw=1
        while true; do
            if [ "$first_draw" = "0" ]; then
                printf "\033[%dA" "$((count + 1))" > /dev/tty
            fi
            first_draw=0

            printf "\033[2K\033[1;36m%s\033[0m\n" "$prompt" > /dev/tty
            local i=0
            while [ "$i" -lt "$count" ]; do
                printf "\033[2K" > /dev/tty
                if [ "$i" -eq "$selected" ]; then
                    printf "  \033[1;32m>\033[0m \033[1m%s\033[0m\n" "${options[$i]}" > /dev/tty
                else
                    printf "    %s\n" "${options[$i]}" > /dev/tty
                fi
                i=$((i + 1))
            done

            key=""
            IFS= read -rsn1 key < /dev/tty || true
            if [ "$key" = "$(printf '\033')" ]; then
                rest=""
                IFS= read -rsn2 -t 0.05 rest < /dev/tty || true
                key="$key$rest"
            fi

            case "$key" in
                "$(printf '\033[A')") selected=$(( (selected - 1 + count) % count )) ;; # up
                "$(printf '\033[B')") selected=$(( (selected + 1) % count )) ;;         # down
                "") break ;;                                                            # Enter
            esac
        done

        printf "\033[?25h" > /dev/tty # show cursor
        stty "$old_stty" < /dev/tty
        trap - INT

        SELECT_RESULT=$selected
    }

    # tty-aware y/N prompt. $2 is the default ("y" or "n") used both when the
    # user just hits Enter and when there's no controlling terminal at all.
    prompt_yes_no() {
        local prompt="$1"
        local default="$2"
        local suffix="[y/N]"
        [ "$default" = "y" ] && suffix="[Y/n]"

        if [ ! -r /dev/tty ] || [ ! -w /dev/tty ]; then
            # No human to actually answer this, so always decline rather than
            # trust $default, which exists for the Enter-key case below, not
            # this one
            return 1
        fi

        printf "\033[1;36m%s %s \033[0m" "$prompt" "$suffix" > /dev/tty
        local reply=""
        IFS= read -r reply < /dev/tty || true
        if [ -z "$reply" ]; then
            [ "$default" = "y" ]
            return
        fi
        case "$reply" in
            y|Y|yes|YES|Yes) return 0 ;;
            *) return 1 ;;
        esac
    }

    # -------------------------------------------------------------
    # GitHub Releases API (no jq dependency, plain grep/sed)
    # -------------------------------------------------------------
    # Fetches $1, sets $GH_API_BODY and $GH_API_CODE. Network failure and a
    # rate limit (403) are fatal no matter who's calling, so this handles
    # those itself; everything else, 404 included, is left for the caller to
    # phrase in its own words since "not found" means something different
    # per endpoint (no such release vs. no releases at all).
    gh_api_get() {
        local url="$1" response
        response=$(curl -sSL -H "Accept: application/vnd.github+json" -H "User-Agent: wfx-install-script" \
            -w '\n%{http_code}' "$url" 2>/dev/null) || {
            error "Couldn't reach GitHub (network unreachable or DNS lookup failed)."
            return 1
        }

        GH_API_CODE="${response##*$'\n'}"
        GH_API_BODY="${response%$'\n'*}"

        if [ "$GH_API_CODE" = "403" ]; then
            error "GitHub API rate limit hit, try again in a bit."
            return 1
        fi
    }

    # $1: tag, or "latest"/empty for the newest release
    fetch_release_json() {
        local tag="$1" url
        if [ -z "$tag" ] || [ "$tag" = "latest" ]; then
            url="$WFX_GH_API/releases/latest"
        else
            url="$WFX_GH_API/releases/tags/$tag"
        fi

        gh_api_get "$url" || return 1

        case "$GH_API_CODE" in
            200)
                printf '%s' "$GH_API_BODY"
                ;;
            404)
                if [ -z "$tag" ] || [ "$tag" = "latest" ]; then
                    error "No releases published yet for Altered-commits/WFX."
                else
                    error "No release '$tag' found for Altered-commits/WFX."
                fi
                return 1
                ;;
            *)
                error "GitHub API returned HTTP $GH_API_CODE looking up release '$tag'."
                return 1
                ;;
        esac
    }

    json_tag_name() {
        printf '%s' "$1" | grep -o '"tag_name" *: *"[^"]*"' | head -1 | sed 's/.*"\([^"]*\)"$/\1/'
    }

    json_asset_urls() {
        printf '%s' "$1" | grep -o '"browser_download_url" *: *"[^"]*"' | sed 's/.*"\(https:[^"]*\)"/\1/'
    }

    # $1: newline-separated asset URLs -> newline-separated target names
    # (basenames matching wfx-linux-*, .sha256 excluded, 'wfx-' prefix stripped)
    targets_from_urls() {
        local url base
        printf '%s\n' "$1" | while IFS= read -r url; do
            [ -z "$url" ] && continue
            base="${url##*/}"
            case "$base" in
                wfx-linux-*.sha256) ;;
                wfx-linux-*) echo "${base#wfx-}" ;;
            esac
        done
    }

    target_description() {
        case "$1" in
            linux-x86_64-v2)         echo "x86-64, safe baseline (any 2009+ Intel/AMD box)" ;;
            linux-x86_64-v3)         echo "x86-64, AVX2 (most current-gen cloud Intel/AMD)" ;;
            linux-arm64-neoverse-n1) echo "ARM64, AWS Graviton2 (t4g family)" ;;
            linux-arm64-neoverse-v1) echo "ARM64, AWS Graviton3 (c7g/m7g family)" ;;
            *)                       echo "" ;;
        esac
    }

    # -------------------------------------------------------------
    # Prerequisites: same compiler/CMake/Ninja every build-from-source mode
    # needs, because a user project's own code still compiles locally
    # regardless of how 'wfx' itself was installed. Covers apt, dnf, pacman,
    # zypper, apk. Checked pacman first since a couple of distros ship more
    # than one of these binaries.
    # -------------------------------------------------------------
    detect_pkg_manager() {
        if command -v pacman > /dev/null 2>&1; then echo "pacman"
        elif command -v apt-get > /dev/null 2>&1; then echo "apt"
        elif command -v dnf > /dev/null 2>&1; then echo "dnf"
        elif command -v zypper > /dev/null 2>&1; then echo "zypper"
        elif command -v apk > /dev/null 2>&1; then echo "apk"
        else echo ""
        fi
    }

    pkg_install_cmd() {
        case "$1:$2" in
            apt:compiler)    echo "sudo apt-get update && sudo apt-get install -y build-essential" ;;
            apt:cmake)       echo "sudo apt-get install -y cmake" ;;
            dnf:compiler)    echo "sudo dnf install -y gcc-c++" ;;
            dnf:cmake)       echo "sudo dnf install -y cmake" ;;
            pacman:compiler) echo "sudo pacman -S --needed base-devel" ;;
            pacman:cmake)    echo "sudo pacman -S --needed cmake" ;;
            zypper:compiler) echo "sudo zypper install -y gcc-c++" ;;
            zypper:cmake)    echo "sudo zypper install -y cmake" ;;
            apk:compiler)    echo "sudo apk add build-base" ;;
            apk:cmake)       echo "sudo apk add cmake" ;;
            *)               echo "" ;;
        esac
    }

    # Only ever called for --prebuilt-interactive: --prebuilt-known callers are
    # scripting their own provisioning and are assumed to already own their
    # toolchain setup, so nothing here checks, suggests, or installs for them
    check_and_offer_prereqs() {
        local missing=""
        command -v cc > /dev/null 2>&1 || command -v gcc > /dev/null 2>&1 || command -v clang > /dev/null 2>&1 || missing="$missing compiler"
        command -v cmake > /dev/null 2>&1 || missing="$missing cmake"

        if command -v ninja > /dev/null 2>&1; then
            info "Ninja found (optional, faster builds for your own project)."
        else
            info "Ninja not found. Optional, CMake falls back to Makefiles fine without it."
        fi

        if [ -z "$missing" ]; then
            info "Compiler and CMake already present."
            return
        fi

        warn "Missing, needed to build your own project's code against WFX:$missing"

        local pm
        pm=$(detect_pkg_manager)
        if [ -z "$pm" ]; then
            warn "Couldn't detect apt/dnf/pacman/zypper/apk. Install the above manually, see docs/getting_started/installation.md#prerequisites."
            return
        fi

        local item cmd
        for item in $missing; do
            cmd=$(pkg_install_cmd "$pm" "$item")
            [ -z "$cmd" ] && continue

            if prompt_yes_no "Install $item now? ($cmd)" "y"; then
                eval "$cmd" || warn "Failed to install $item, install it manually: $cmd"
            else
                warn "Skipped $item, install it yourself later: $cmd"
            fi
        done
    }

    # -------------------------------------------------------------
    # Version / target selection
    # -------------------------------------------------------------
    prebuilt_pick_version() {
        info "Looking up releases..."
        # Capped: interactive_select redraws by cursor-up'ing exactly
        # count+1 lines, no scrolling, so a long list would garble past
        # whatever the terminal's visible height is. Anything older than
        # this still reachable through "Type a version manually" below.
        gh_api_get "$WFX_GH_API/releases?per_page=5" || return 1
        if [ "$GH_API_CODE" != "200" ]; then
            error "GitHub API returned HTTP $GH_API_CODE listing releases."
            return 1
        fi

        local tags
        tags=$(printf '%s' "$GH_API_BODY" | grep -o '"tag_name" *: *"[^"]*"' | sed 's/.*"\([^"]*\)"$/\1/')
        if [ -z "$tags" ]; then
            error "No releases published yet for Altered-commits/WFX."
            return 1
        fi

        local labels=() first=1 t
        while IFS= read -r t; do
            [ -z "$t" ] && continue
            if [ "$first" = "1" ]; then
                labels+=("$t (latest, recommended)")
                first=0
            else
                labels+=("$t")
            fi
        done <<TAGS
$tags
TAGS
        labels+=("Type a version manually")

        interactive_select "Which WFX version do you want?" "${labels[@]}"
        local idx=$SELECT_RESULT
        local count=${#labels[@]}
        local picked

        if [ "$idx" -eq "$((count - 1))" ]; then
            if [ -r /dev/tty ]; then
                printf "\033[1;36mEnter a version tag (e.g. v1.2.3): \033[0m" > /dev/tty
                read -r picked < /dev/tty
            else
                error "No terminal to read a manual version from."
                return 1
            fi
        else
            picked=$(printf '%s\n' "$tags" | sed -n "$((idx + 1))p")
        fi

        echo "$picked"
    }

    # $1: newline-separated target names
    prebuilt_pick_target() {
        local target_list="$1"
        local host_arch
        host_arch=$(uname -m)

        local tlabels=() ttargets=() t desc
        while IFS= read -r t; do
            [ -z "$t" ] && continue
            desc=$(target_description "$t")
            if [ -n "$desc" ]; then
                tlabels+=("$t - $desc")
            else
                tlabels+=("$t")
            fi
            ttargets+=("$t")
        done <<TARGETS
$target_list
TARGETS

        interactive_select "Which build? (this machine reports: $host_arch)" "${tlabels[@]}"
        echo "${ttargets[$SELECT_RESULT]}"
    }

    # -------------------------------------------------------------
    # Orchestration
    # -------------------------------------------------------------
    run_prebuilt_install() {
        local version target

        if [ "$PREBUILT_MODE" = "known" ]; then
            version="$PREBUILT_VERSION"
            target="$PREBUILT_TARGET"
        else
            version=$(prebuilt_pick_version) || exit 1
        fi

        info "Looking up release '$version'..."
        local release_json
        release_json=$(fetch_release_json "$version") || exit 1

        local resolved_version
        resolved_version=$(json_tag_name "$release_json")
        [ -n "$resolved_version" ] && version="$resolved_version"

        local urls
        urls=$(json_asset_urls "$release_json")
        [ -n "$urls" ] || fatal "Release '$version' has no downloadable assets."

        local headers_url
        headers_url=$(printf '%s\n' "$urls" | grep '/wfx-headers\.tar\.gz$' | head -1)
        [ -n "$headers_url" ] || fatal "Release '$version' has no wfx-headers.tar.gz asset."

        if [ "$PREBUILT_MODE" != "known" ]; then
            local target_list
            target_list=$(targets_from_urls "$urls")
            [ -n "$target_list" ] || fatal "Release '$version' has no target binaries published."
            target=$(prebuilt_pick_target "$target_list") || exit 1
        fi

        local binary_url
        binary_url=$(printf '%s\n' "$urls" | grep "/wfx-${target}\$" | head -1)
        if [ -z "$binary_url" ]; then
            local available
            available=$(targets_from_urls "$urls" | tr '\n' ' ')
            fatal "No 'wfx-$target' asset on release '$version'. Available targets: $available"
        fi

        info "Version: $version"
        info "Target:  $target"

        local tmpdir
        tmpdir=$(mktemp -d) || fatal "Couldn't create a temp directory."
        trap 'rm -rf "$tmpdir"' EXIT

        info "Downloading wfx-$target..."
        curl -fsSL -o "$tmpdir/wfx-$target" "$binary_url" || fatal "Failed to download $binary_url"
        curl -fsSL -o "$tmpdir/wfx-$target.sha256" "$binary_url.sha256" || fatal "Failed to download $binary_url.sha256"
        ( cd "$tmpdir" && sha256sum -c "wfx-$target.sha256" > /dev/null ) \
            || fatal "Checksum verification failed for wfx-$target, aborting."

        info "Downloading wfx-headers.tar.gz..."
        curl -fsSL -o "$tmpdir/wfx-headers.tar.gz" "$headers_url" || fatal "Failed to download $headers_url"
        curl -fsSL -o "$tmpdir/wfx-headers.tar.gz.sha256" "$headers_url.sha256" || fatal "Failed to download $headers_url.sha256"
        ( cd "$tmpdir" && sha256sum -c "wfx-headers.tar.gz.sha256" > /dev/null ) \
            || fatal "Checksum verification failed for wfx-headers.tar.gz, aborting."

        info "Setting up ~/.wfx directory structure..."
        mkdir -p "$WFX_BIN" "$WFX_SRC" "$WFX_HOME/daemons"

        rm -f "$WFX_BINARY"
        mv "$tmpdir/wfx-$target" "$WFX_BINARY" || fatal "Failed to move binary to $WFX_BIN."
        chmod +x "$WFX_BINARY"

        # include/, shared/ and scripts/uninstall.sh replace cleanly on a
        # re-run (e.g. moving to a newer version), everything else under
        # $WFX_SRC is left alone
        rm -rf "$WFX_SRC/include" "$WFX_SRC/shared" "$WFX_SRC/scripts"
        tar -xzf "$tmpdir/wfx-headers.tar.gz" -C "$WFX_SRC" || fatal "Failed to extract headers into $WFX_SRC."

        echo "$REQUESTED_TYPE" > "$WFX_INSTALL_TYPE_FILE"

        # --prebuilt-known callers own their own provisioning, nothing to check or offer here
        [ "$PREBUILT_MODE" = "interactive" ] && check_and_offer_prereqs
        setup_path
        print_success
    }

    run_prebuilt_install
    exit 0
fi

# =================================================================
# Build from source (no flag / --local-debug / --local-release)
# =================================================================

# ---------------------------------------------------------------
# Check dependencies
# ---------------------------------------------------------------
check_dep() {
    if ! command -v "$1" > /dev/null 2>&1; then
        fatal "Required dependency '$1' not found. $2"
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
    # Clear out whatever end-user (or older --local) state might already be
    # there - a plain real directory has nothing worth preserving (it's
    # entirely install-generated), and 'ln -sfn' can't replace one on its own
    if [ -d "$WFX_SRC" ] && [ ! -L "$WFX_SRC" ]; then
        rm -rf "$WFX_SRC"
    fi
    ln -sfn "$REPO_ROOT" "$WFX_SRC" || fatal "Failed to link $WFX_SRC to $REPO_ROOT."
else
    # Clear out a stale local-mode symlink before treating $WFX_SRC as a real
    # clone target below
    if [ -L "$WFX_SRC" ]; then
        rm -f "$WFX_SRC"
    fi
    mkdir -p "$WFX_SRC"

    if [ -d "$WFX_SRC/.git" ]; then
        info "Source already exists, updating..."
        git -C "$WFX_SRC" pull --ff-only || fatal "Failed to update WFX source. Resolve conflicts in $WFX_SRC manually."
    else
        info "Cloning WFX repository..."
        git clone --depth=1 "$WFX_REPO" "$WFX_SRC" || fatal "Failed to clone WFX repository."
    fi
fi

# ---------------------------------------------------------------
# Build
# ---------------------------------------------------------------
info "Configuring build..."
if [ -n "$LOCAL_MODE" ]; then
    # Configure against the real checkout, not the $WFX_SRC symlink. CMake bakes
    # whatever '-S' it's given verbatim into compile_commands.json's file paths,
    # so configuring via the symlink poisons it for any tool (clang-tidy) that
    # does exact-string path matching against the real checkout path
    SOURCE_DIR="$REPO_ROOT"
    # Same build dir for both local modes - switching between them just makes
    # CMake reconfigure and Ninja recompile whatever the flag change touches
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
    || fatal "CMake configuration failed."

info "Building WFX (this may take a moment)..."
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" \
    || fatal "Build failed."

# ---------------------------------------------------------------
# Install binary
# ---------------------------------------------------------------
info "Installing binary to $WFX_BINARY..."
# CMake always drops the built binary at the source root ($WFX_SRC/wfx),
# same for both modes. $WFX_BIN itself is never a symlink, only $WFX_SRC
# is (in local mode) - remove whatever's currently at $WFX_BINARY first
# since 'mv' onto an existing symlink would write through it instead of
# replacing the link itself
rm -f "$WFX_BINARY"
mv "$WFX_SRC/wfx" "$WFX_BINARY" || fatal "Failed to move binary to $WFX_BIN."
chmod +x "$WFX_BINARY"

# Record the install type so future runs on this machine are locked to it
# (see the enforcement check above)
echo "$REQUESTED_TYPE" > "$WFX_INSTALL_TYPE_FILE"

setup_path
print_success
