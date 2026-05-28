#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------
# Project clang-format helper
#
# Usage:
#   ./scripts/format.sh
#   ./scripts/format.sh --dry-run
#   ./scripts/format.sh --files path/to/file.cpp path/to/other.hpp
#   ./scripts/format.sh --dry-run --files path/to/file.cpp
#
# Modes:
#   default     -> modifies files in-place
#   --dry-run   -> validates formatting, shows diff of what clang-format wants
#
# Options:
#   --files     -> run only on the specified files instead of scanning the repo
# ---------------------------------------------------------------

# ---------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

SOURCE_EXTENSIONS=(
    "cpp"
    "hpp"
    "ipp"
)

IGNORE_DIRECTORIES=(
    "./build"
    "./test"
    "./.git"
)

# ---------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------
info()    { printf "\033[1;34m[FORMAT]\033[0m %s\n" "$*"; }
success() { printf "\033[1;32m[FORMAT]\033[0m %s\n" "$*"; }
warn()    { printf "\033[1;33m[FORMAT]\033[0m %s\n" "$*"; }
error()   { printf "\033[1;31m[FORMAT]\033[0m %s\n" "$*" >&2; exit 1; }

# ---------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------
MODE="format"
CUSTOM_FILES=()
FILES_MODE=0

i=1
while [ "$i" -le "$#" ]; do
    arg="${!i}"
    case "$arg" in
        --dry-run)
            MODE="dry-run"
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
check_dep "$CLANG_FORMAT"
check_dep diff

# ---------------------------------------------------------------
# Show clang-format version and config file
# ---------------------------------------------------------------
CLANG_VERSION=$("$CLANG_FORMAT" --version)
info "Formatter     : $CLANG_VERSION"

# Find which .clang-format file will be used (walks up from cwd)
CONFIG_FILE=""
SEARCH_DIR="$(pwd)"
while [ "$SEARCH_DIR" != "/" ]; do
    if [ -f "$SEARCH_DIR/.clang-format" ]; then
        CONFIG_FILE="$SEARCH_DIR/.clang-format"
        break
    fi
    SEARCH_DIR="$(dirname "$SEARCH_DIR")"
done

if [ -n "$CONFIG_FILE" ]; then
    info "Config file   : $CONFIG_FILE"
else
    warn "No .clang-format file found, clang-format will use defaults"
fi

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
else
    # Build find arguments
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
# Run clang-format
# ---------------------------------------------------------------
info "Mode          : $MODE"
info "Files matched : $FILE_COUNT"
echo ""

FAILURES=0
PROCESSED=0

for file in "${FILES[@]}"; do
    printf "[%d/%d] %s\n" "$((PROCESSED + 1))" "$FILE_COUNT" "$file"

    if [ "$MODE" = "dry-run" ]; then
        FORMATTED=$("$CLANG_FORMAT" "$file")
        ORIGINAL=$(cat "$file")

        if [ "$FORMATTED" != "$ORIGINAL" ]; then
            FAILURES=$((FAILURES + 1))
            echo ""
            printf "\033[1;33m  Violations in: %s\033[0m\n" "$file"
            echo "  --- original"
            echo "  +++ clang-format wants"
            diff --unified=3 <(echo "$ORIGINAL") <(echo "$FORMATTED") \
                | tail -n +3 \
                | sed 's/^/  /' \
                || true
            echo ""
        fi
    else
        "$CLANG_FORMAT" -i "$file"
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

if [ "$MODE" = "dry-run" ]; then
    if [ "$FAILURES" -ne 0 ]; then
        echo ""
        warn "Files with violations : $FAILURES"
        warn "Run './scripts/format.sh' to apply fixes automatically."
        exit 1
    fi

    success "All files are correctly formatted."
else
    success "Formatting completed successfully."
fi