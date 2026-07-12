#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------
# Project clang-tidy helper
#
# Usage:
#   ./scripts/tidy.sh                      -> lint every project source file
#   ./scripts/tidy.sh --fix                -> lint AND apply auto-fixes in-place
#   ./scripts/tidy.sh --changed            -> lint only files changed vs 'main'
#   ./scripts/tidy.sh --files a.cpp b.hpp  -> lint only the given files
#   ./scripts/tidy.sh --ci                 -> CI mode, see below
#
# Requires a compile database, built directly in THIS checkout - never via
# '~/.wfx/src' (that's a separate rsync mirror produced by
# './scripts/install.sh --local'; running clang-tidy against a mirror's
# compile_commands.json meant '--fix' edited files under '~/.wfx/src'
# instead of this working tree). clang-tidy is invoked with '-p <build-dir>'
# and looks up each file's flags itself, so '--fix' writes straight to the
# working tree.
#
#   Local (default): './build_tidy', configured by this script on first run
#     (config only, no full build - compile_commands.json is emitted at
#     CMake configure time). Reused on later runs.
#
#   --ci: './build', produced by compile_check.yml's clang leg in the SAME
#     checkout this runs in. CI restores that job's cache before calling
#     this with --ci, so nothing gets rebuilt here either
# ---------------------------------------------------------------

# ---------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
LOCAL_BUILD_DIR="build_tidy"
CI_BUILD_DIR="build"

SOURCE_EXTENSIONS=(
    "cpp"
)

IGNORE_DIRECTORIES=(
    "./.git"
    "./.github"
    "./.venv"
    "./.vscode"
    "./build"
    "./build_tidy"
    "./docs"
    "./tests"
)

# ---------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------
info()    { printf "\033[1;34m[TIDY]\033[0m %s\n" "$*"; }
success() { printf "\033[1;32m[TIDY]\033[0m %s\n" "$*"; }
warn()    { printf "\033[1;33m[TIDY]\033[0m %s\n" "$*"; }
error()   { printf "\033[1;31m[TIDY]\033[0m %s\n" "$*" >&2; exit 1; }

# ---------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------
MODE="check"
CUSTOM_FILES=()
FILES_MODE=0
CHANGED=0
CI=0

i=1
while [ "$i" -le "$#" ]; do
    arg="${!i}"
    case "$arg" in
        --fix)
            MODE="fix"
            ;;
        --changed)
            CHANGED=1
            ;;
        --ci)
            CI=1
            ;;
        --files)
            FILES_MODE=1
            ;;
        -*)
            error "Unknown argument: $arg"
            ;;
        *)
            if [ "$FILES_MODE" = "1" ]; then
                CUSTOM_FILES+=("$arg")
            else
                error "Unknown argument: $arg"
            fi
            ;;
    esac
    i=$((i + 1))
done

# ---------------------------------------------------------------
# Dependency checks
# ---------------------------------------------------------------
check_dep() {
    if ! command -v "$1" > /dev/null 2>&1; then
        error "Required dependency '$1' not found."
    fi
}

info "Checking dependencies..."
check_dep "$CLANG_TIDY"
check_dep git

TIDY_VERSION=$("$CLANG_TIDY" --version | head -n1)
info "Tidy          : $TIDY_VERSION"

if [ -f ".clang-tidy" ]; then
    info "Config file   : $(pwd)/.clang-tidy"
else
    warn "No .clang-tidy file found at repo root, clang-tidy will use built-in defaults"
fi

echo ""

# ---------------------------------------------------------------
# Confirm the build we're reusing exists
# ---------------------------------------------------------------
if [ "$CI" = "1" ]; then
    BUILD_DIR="$CI_BUILD_DIR"
else
    BUILD_DIR="$LOCAL_BUILD_DIR"
fi

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
    if [ "$CI" = "1" ]; then
        error "No build found at $BUILD_DIR. The calling CI workflow must build it first."
    else
        info "No compile database at $BUILD_DIR yet, configuring (no full build needed)..."
        GENERATOR="Unix Makefiles"
        command -v ninja > /dev/null 2>&1 && GENERATOR="Ninja"
        cmake -S . -B "$BUILD_DIR" -G "$GENERATOR" -DCMAKE_BUILD_TYPE=Release \
            || error "CMake configuration failed for $BUILD_DIR."
        echo ""
    fi
fi

info "Reusing build : $BUILD_DIR"
echo ""

# ---------------------------------------------------------------
# Collect files
# ---------------------------------------------------------------
FILES=()

if [ "${#CUSTOM_FILES[@]}" -gt 0 ]; then
    for f in "${CUSTOM_FILES[@]}"; do
        if [ ! -f "$f" ]; then
            warn "File not found, skipping: $f"
            continue
        fi
        FILES+=("$f")
    done
elif [ "$CHANGED" = "1" ]; then
    BASE_REF=$(git merge-base HEAD main 2>/dev/null || git merge-base HEAD origin/main 2>/dev/null || echo "")
    if [ -z "$BASE_REF" ]; then
        error "Could not determine a merge-base against 'main'/'origin/main' for --changed."
    fi

    while IFS= read -r file; do
        [ -f "$file" ] || continue
        case "$file" in
            *.cpp) FILES+=("$file") ;;
        esac
    done < <(git diff --name-only "$BASE_REF" -- '*.cpp')
else
    FIND_ARGS=()

    if [ ${#IGNORE_DIRECTORIES[@]} -gt 0 ]; then
        FIND_ARGS+=( "(" )

        for i in "${!IGNORE_DIRECTORIES[@]}"; do
            FIND_ARGS+=( -path "${IGNORE_DIRECTORIES[$i]}" )

            if [ "$i" -lt $((${#IGNORE_DIRECTORIES[@]} - 1)) ]; then
                FIND_ARGS+=( -o )
            fi
        done

        FIND_ARGS+=( ")" -prune -o )
    fi

    FIND_ARGS+=( "(" )

    for i in "${!SOURCE_EXTENSIONS[@]}"; do
        FIND_ARGS+=( -name "*.${SOURCE_EXTENSIONS[$i]}" )

        if [ "$i" -lt $((${#SOURCE_EXTENSIONS[@]} - 1)) ]; then
            FIND_ARGS+=( -o )
        fi
    done

    FIND_ARGS+=( ")" -print0 )

    while IFS= read -r -d '' file; do
        FILES+=("$file")
    done < <(find . "${FIND_ARGS[@]}")
fi

FILE_COUNT=${#FILES[@]}

if [ "$FILE_COUNT" -eq 0 ]; then
    warn "No matching source files found."
    exit 0
fi

# ---------------------------------------------------------------
# Run clang-tidy
# ---------------------------------------------------------------
info "Mode          : $MODE"
info "Files matched : $FILE_COUNT"
echo ""

TIDY_ARGS=(--quiet -p "$BUILD_DIR")
if [ "$MODE" = "fix" ]; then
    TIDY_ARGS+=(--fix --fix-errors)
fi

FAILURES=0
PROCESSED=0

for file in "${FILES[@]}"; do
    printf "[%d/%d] %s\n" "$((PROCESSED + 1))" "$FILE_COUNT" "$file"

    # 'clang-tidy' exits 0 even with findings unless WarningsAsErrors matched, so-
    # -detect findings from output content, not just the exit code. '-p' looks up-
    # -flags for '$file' itself from the compile database (headers fall back to-
    # -the flags of a .cpp in the same directory automatically)
    STATUS=0
    OUTPUT=$("$CLANG_TIDY" "${TIDY_ARGS[@]}" "$file" 2>&1) || STATUS=$?

    if [ -n "$OUTPUT" ]; then
        echo "$OUTPUT"
    fi

    if [ "$STATUS" -ne 0 ] || printf '%s' "$OUTPUT" | grep -qE '(warning|error): '; then
        FAILURES=$((FAILURES + 1))
    fi

    PROCESSED=$((PROCESSED + 1))
done

# ---------------------------------------------------------------
# Summary
# ---------------------------------------------------------------
echo ""
printf '%0.s-' {1..60}
echo ""

success "Processed files : $PROCESSED"

if [ "$FAILURES" -ne 0 ]; then
    echo ""
    warn "Files with findings : $FAILURES"
    if [ "$MODE" != "fix" ]; then
        warn "Run './scripts/tidy.sh --fix' to apply auto-fixes where possible."
    fi
    exit 1
fi

success "No issues found."
