#!/usr/bin/env bash
# wrk every feature route @ 10k connections (one route per fresh server start)
# Usage: ./scripts/bench_10k_all_routes.sh
set -euo pipefail

export WRK_THREADS="${WRK_THREADS:-4}"
export WRK_CONNECTIONS="${WRK_CONNECTIONS:-10000}"
export WRK_DURATION="${WRK_DURATION:-20s}"
export WRK_TIMEOUT="${WRK_TIMEOUT:-10s}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec bash "${ROOT}/scripts/test_full_routes.sh"
