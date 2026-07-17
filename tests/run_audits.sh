#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025-2026 Altered-commits
#
# Single entry point for running the WFX audits (base, endpoint, tls, crypto),
# locally or in CI.
#
# This script does NOT build wfx. It assumes a wfx binary already exists,
# either on PATH or at the repo root (where a normal CMake build leaves it).
# Build it yourself first, with ./scripts/install.sh --local-debug or a plain
# cmake/ninja build, the same way you would before running any audit by hand.
#
# Usage:
#   tests/run_audits.sh                  run all four audits, one after another
#   tests/run_audits.sh --audit base     run one audit only: base, endpoint, tls, crypto
#   tests/run_audits.sh --ci             forward --ci to every audit that runs
#   tests/run_audits.sh --audit tls -- --phase verify --wfx-logs all
#                                        anything after -- is passed through as-is
#
# Locally, run it with no --audit and it goes through all four in sequence,
# which is what you want on a dev machine. In GitHub Actions, --audit is set
# to one name per matrix job, so the four run as separate parallel jobs
# instead, see .github/workflows/audit_check.yml.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

audit=""
ci=0
extra_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --audit)
            audit="$2"
            shift 2
            ;;
        --ci)
            ci=1
            shift
            ;;
        --)
            shift
            extra_args=("$@")
            break
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

declare -A AUDIT_DIRS=(
    [base]="base_audit"
    [endpoint]="endpoint_audit"
    [tls]="tls_audit"
    [crypto]="crypto_audit"
)
declare -A AUDIT_SCRIPTS=(
    [base]="base_audit.py"
    [endpoint]="endpoint_audit.py"
    [tls]="tls_audit.py"
    [crypto]="crypto_audit.py"
)

# Every audit's --wfx defaults to "wfx" on PATH. The binary itself lands at
# the repo root (CMAKE_RUNTIME_OUTPUT_DIRECTORY), not on PATH, so point at it
# directly unless the caller already has a real "wfx" available or passed
# their own --wfx through extra_args.
wfx_bin="wfx"
if ! command -v wfx >/dev/null 2>&1 && [[ -x "$REPO_ROOT/wfx" ]]; then
    wfx_bin="$REPO_ROOT/wfx"
fi

run_one() {
    local name="$1"
    local dir="${AUDIT_DIRS[$name]:-}"
    if [[ -z "$dir" ]]; then
        echo "Unknown audit: $name (expected one of: base, endpoint, tls, crypto)" >&2
        return 1
    fi

    local args=(--wfx "$wfx_bin")
    [[ $ci -eq 1 ]] && args+=(--ci)
    args+=("${extra_args[@]}")

    # CI always starts from a clean checkout, so its template cache is never stale
    # Locally it's gitignored and persists across runs, which can hide a template-
    # -engine bug that a real recompile would've caught. Delete just the cache-
    # -file so every run (local or CI) recompiles templates from current source
    rm -f "$REPO_ROOT/tests/$dir/app/intermediate/template.wfxmeta"

    echo "==> running $name audit"
    (cd "$REPO_ROOT/tests/$dir" && python3 "${AUDIT_SCRIPTS[$name]}" "${args[@]}")
}

if [[ -n "$audit" ]]; then
    run_one "$audit" || exit $?
    exit 0
fi

failed=()
for name in base endpoint tls crypto; do
    if ! run_one "$name"; then
        failed+=("$name")
    fi
done

echo
if [[ ${#failed[@]} -eq 0 ]]; then
    echo "All audits passed."
    exit 0
else
    echo "Failed audits: ${failed[*]}"
    exit 1
fi
