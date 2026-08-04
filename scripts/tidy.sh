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
# Always uses THIS checkout's own './build', never '~/.wfx/src' (that's a symlink into a-
# -checkout for './scripts/install.sh --local-debug'/'--local-release', or a separate real-
# -clone for a plain end-user install; either way its own compile_commands.json can point-
# -somewhere other than here). clang-tidy is invoked with '-p build' and looks up each-
# -file's flags itself, so '--fix' always writes straight to this working tree.
#
# Locally: if './build' doesn't exist yet, this configures it (fast, no full build,-
# -compile_commands.json is emitted at CMake configure time). Files that need real-
# -OpenSSL headers (anything using recent APIs) won't fully resolve until something-
# -actually builds it (e.g. running './scripts/install.sh --local-debug', the same-
# -'./build'), so once it's been run once, tidy sees the real thing from then on.
#
# --ci: same './build', but produced by compile_check.yml's clang leg in the SAME-
# -checkout this runs in (a full build, so OpenSSL is always real there). CI restores-
# -that job's cache before calling this with --ci, so nothing gets rebuilt here either
# ---------------------------------------------------------------

# ---------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
BUILD_DIR="build"

# scripts/py/tidy_cache.py: skips re-running clang-tidy on a file whose preprocessed-
# -content + resolved config + args are unchanged, replaying the exact prior-
# -stdout/exit code instead. Zero effect on which checks run or what they find,-
# -only on whether an unchanged file gets re-analyzed.
# Disabled for --fix: a cache hit skips invoking real clang-tidy entirely, which-
# -would skip the in-place edit too, so --fix must always run for real.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIDY_CACHE_PY="$SCRIPT_DIR/py/tidy_cache.py"
CTCACHE_ENABLE="${CTCACHE_ENABLE:-1}"

SOURCE_EXTENSIONS=(
    "cpp"
)

IGNORE_DIRECTORIES=(
    "./.git"
    "./.github"
    "./.venv"
    "./.vscode"
    "./build"
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

if [ "$CTCACHE_ENABLE" = "1" ] && [ "$MODE" != "fix" ]; then
    if command -v python3 > /dev/null 2>&1; then
        USE_CACHE=1
    else
        warn "python3 not found, disabling clang-tidy result cache for this run"
        USE_CACHE=0
    fi
else
    USE_CACHE=0
fi

TIDY_VERSION=$("$CLANG_TIDY" --version | head -n1)
info "Tidy          : $TIDY_VERSION"
if [ "$USE_CACHE" = "1" ]; then
    info "Result cache  : enabled ($TIDY_CACHE_PY)"
fi

if [ -f ".clang-tidy" ]; then
    info "Config file   : $(pwd)/.clang-tidy"
else
    warn "No .clang-tidy file found at repo root, clang-tidy will use built-in defaults"
fi

echo ""

# ---------------------------------------------------------------
# Confirm the build we're reusing exists
# ---------------------------------------------------------------
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

# CMake's own configure step only declares the OpenSSL dependency, it doesn't build it-
# -(ExternalProject_Add builds happen at 'cmake --build' time), so a configure-only-
# -$BUILD_DIR (i.e. nobody has run a full build here yet) leaves openssl_lts-install/-
# -include empty, and clang-tidy silently falls back to whatever OpenSSL the system-
# -happens to have (often older, missing newer APIs). Not fatal - only files that touch-
# -those newer APIs are affected - so this is a warning, not a hard stop
if [ "$CI" != "1" ] && [ ! -f "$BUILD_DIR/openssl_lts-install/include/openssl/core_names.h" ]; then
    warn "OpenSSL not fully built in $BUILD_DIR yet - files using recent OpenSSL APIs may show" \
        "false errors. Run './scripts/install.sh --local-debug' (or any full build) once to fix this."
    echo ""
fi

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

# Each file is an independent clang-tidy invocation (own process, own AST, no-
# -shared state), so running them one at a time wastes wall-clock time for no-
# -reason. --fix stays sequential (JOBS=1): concurrent in-place '--fix' runs-
# -could corrupt a header shared by multiple TUs.
NPROC=$(command -v nproc > /dev/null 2>&1 && nproc || echo 4)
if [ "$MODE" = "fix" ]; then
    JOBS=1
elif [ "$CI" = "1" ]; then
    # GitHub-hosted runners are small (commonly 4 vCPU) and shared/throttled.
    # clang-analyzer checks are heavy enough per-process that matching nproc-
    # -exactly causes memory/scheduler thrashing instead of speeding things up.
    JOBS="${TIDY_JOBS:-$((NPROC > 3 ? 3 : NPROC))}"
else
    # Same reasoning applies locally: clang-analyzer is heavy enough per-file-
    # -that matching nproc exactly tends to thrash rather than help. Cap it,-
    # -override with TIDY_JOBS=<n> for something different.
    JOBS="${TIDY_JOBS:-$((NPROC > 8 ? 8 : NPROC))}"
fi
info "Jobs          : $JOBS"
echo ""

RESULT_DIR=$(mktemp -d)
trap 'rm -rf "$RESULT_DIR"' EXIT

run_one() {
    local idx="$1" file="$2"
    local status=0
    local output

    # 'clang-tidy' exits 0 even with findings unless WarningsAsErrors matched, so-
    # -detect findings from output content, not just the exit code. '-p' looks up-
    # -flags for '$file' itself from the compile database (headers fall back to-
    # -the flags of a .cpp in the same directory automatically)
    if [ "$USE_CACHE" = "1" ]; then
        output=$(python3 "$TIDY_CACHE_PY" "$CLANG_TIDY" "${TIDY_ARGS[@]}" "$file" 2>&1) || status=$?
    else
        output=$("$CLANG_TIDY" "${TIDY_ARGS[@]}" "$file" 2>&1) || status=$?
    fi

    # Clang's diagnostics engine prints its own "N warnings generated." tally per-
    # -translation unit it processes internally, unrelated to clang-tidy's own findings-
    # -and not suppressed by --quiet, strip it so only real findings show
    #
    # Here-strings, not 'printf | grep': with pipefail, grep -q's early exit on a match-
    # -can SIGPIPE a still-writing printf on large output, and pipefail then reports-
    # -that non-zero death as the pipeline's status instead of grep's real match
    output=$(grep -vE '^[0-9]+ warnings? generated\.$' <<< "$output" || true)

    printf '%s' "$output" > "$RESULT_DIR/$idx.out"
    if [ "$status" -ne 0 ] || grep -qE '(warning|error): ' <<< "$output"; then
        : > "$RESULT_DIR/$idx.fail"
    fi
}

RUNNING=0
IDX=0
for file in "${FILES[@]}"; do
    # Printed as each file is handed to a worker, not when it finishes, so-
    # -local runs still show live progress instead of going quiet until the-
    # -whole batch completes.
    printf "[%d/%d] %s\n" "$((IDX + 1))" "$FILE_COUNT" "$file"
    run_one "$IDX" "$file" &
    RUNNING=$((RUNNING + 1))
    IDX=$((IDX + 1))
    if [ "$RUNNING" -ge "$JOBS" ]; then
        wait -n
        RUNNING=$((RUNNING - 1))
    fi
done
wait
echo ""

FAILURES=0
PROCESSED=0
for file in "${FILES[@]}"; do
    # Filename already printed once above at dispatch time; only show output here,-
    # -in original file order, for whichever files actually had findings
    if [ -s "$RESULT_DIR/$PROCESSED.out" ]; then
        cat "$RESULT_DIR/$PROCESSED.out"
        echo ""
    fi

    if [ -f "$RESULT_DIR/$PROCESSED.fail" ]; then
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
