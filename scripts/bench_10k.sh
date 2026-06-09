#!/usr/bin/env bash
# WFX 10k-connection benchmark — restarts server each run for stable numbers
# Usage: ./scripts/bench_10k.sh [runs] [duration_sec]
set -euo pipefail

RUNS="${1:-5}"
DURATION="${2:-30}"
PROJECT="wfx_loadtest"
URL="http://127.0.0.1:8080/text"
WFX_BIN="${WFX_BIN:-./wfx}"
WRK_BIN="${WRK_BIN:-wrk}"
THREADS="${WRK_THREADS:-10}"

ulimit -n 65536 2>/dev/null || true

killall -9 wfx 2>/dev/null || true
pkill -9 -f "wfx run ${PROJECT}" 2>/dev/null || true
sleep 1

echo "[bench] Building..."
"$WFX_BIN" build "$PROJECT" source >/dev/null

echo "[bench] ${RUNS} runs × ${DURATION}s | threads=${THREADS} | connections=10000"
echo "[bench] (server restarted before each run for stable results)"
RESULTS=()

for i in $(seq 1 "$RUNS"); do
    pkill -9 -f "wfx run ${PROJECT}" 2>/dev/null || true
    sleep 2

    "$WFX_BIN" run "$PROJECT" >/dev/null 2>&1 &
    SERVER_PID=$!

    for _ in $(seq 1 40); do
        curl -sf "$URL" >/dev/null 2>&1 && break
        sleep 0.25
    done

    OUT=$("$WRK_BIN" -t"$THREADS" -c10000 -d"${DURATION}s" --timeout 10s "$URL" 2>&1)
    RPS=$(echo "$OUT" | awk '/Requests\/sec/ {print $2}')
    ERR=$(echo "$OUT" | grep "Socket errors" || true)
    echo "  run $i: ${RPS} req/s | ${ERR}"
    RESULTS+=("$RPS")

    kill -9 "$SERVER_PID" 2>/dev/null || true
    pkill -9 -f "wfx run ${PROJECT}" 2>/dev/null || true
    sleep 2
done

echo ""
echo "[bench] Summary:"
printf '  %s\n' "${RESULTS[@]}" | awk '
    { sum += $1; if (NR==1 || $1 < min) min=$1; if (NR==1 || $1 > max) max=$1 }
    END {
        if (NR > 0)
            printf "  min=%.2f  avg=%.2f  max=%.2f  runs=%d\n", min, sum/NR, max, NR
    }'
