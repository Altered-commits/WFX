#!/usr/bin/env bash
# WFX integration test suite (macOS)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="${WFX_PROJECT:-wfx_loadtest}"
WFX="${ROOT}/wfx"
PORT=8080
BASE="http://127.0.0.1:${PORT}"
PASS=0
FAIL=0
SERVER_PID=""

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

stop_server() {
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    SERVER_PID=""
    pkill -f "wfx run ${PROJECT}" 2>/dev/null || true
    sleep 1
}

start_server() {
    local workers=$1
    stop_server
    sed -i '' "s/worker_processes        = .*/worker_processes        = ${workers}/" "${ROOT}/${PROJECT}/wfx.toml"
    if [[ "${workers}" -gt 1 ]]; then
        sed -i '' 's/max_connections              = .*/max_connections              = 3072/' "${ROOT}/${PROJECT}/wfx.toml"
    else
        sed -i '' 's/max_connections              = .*/max_connections              = 12032/' "${ROOT}/${PROJECT}/wfx.toml"
    fi
    ulimit -n 65536 2>/dev/null || true
    cd "${ROOT}"
    "${WFX}" build "${PROJECT}" source >/dev/null
    "${WFX}" run "${PROJECT}" >/dev/null 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 40); do
        curl -sf "${BASE}/text" >/dev/null 2>&1 && return 0
        sleep 0.25
    done
    fail "Server failed to start (workers=${workers})"
    return 1
}

assert_status() {
    local method=$1 path=$2 expected=$3 label=$4
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X "${method}" "${BASE}${path}" 2>/dev/null || true)
    if [[ "${code}" == "${expected}" ]]; then
        pass "${label} (${code})"
    else
        fail "${label} (expected ${expected}, got ${code})"
    fi
}

assert_body_contains() {
    local path=$1 needle=$2 label=$3
    local body
    body=$(curl -sf "${BASE}${path}" 2>/dev/null || echo "")
    if echo "${body}" | grep -q "${needle}"; then
        pass "${label}"
    else
        fail "${label} (body missing '${needle}')"
    fi
}

assert_json_field() {
    local path=$1 needle=$2 label=$3
    local body
    body=$(curl -sf "${BASE}${path}" 2>/dev/null || echo "")
    if echo "${body}" | grep -q "${needle}"; then
        pass "${label}"
    else
        fail "${label}"
    fi
}

test_endpoints() {
    echo ""
    echo "== Endpoint tests =="
    assert_body_contains "/text" "Hello from WFX" "GET /text"
    assert_json_field "/im-json" "WFX" "GET /im-json"
    assert_json_field "/rm-json" "WEIRD" "GET /rm-json"
    assert_body_contains "/template" "Hello from WFX Template" "GET /template"
    assert_status GET "/not-found" 404 "GET /not-found 404"
    assert_status POST "/text" 404 "POST /text unsupported"
}

test_keepalive() {
    echo ""
    echo "== Keep-alive =="
    local ok=0
    for _ in $(seq 1 20); do
        curl -sf "${BASE}/text" >/dev/null 2>&1 && ok=$((ok + 1))
    done
    if [[ ${ok} -eq 20 ]]; then
        pass "20 sequential requests"
    else
        fail "20 sequential requests (${ok}/20 succeeded)"
    fi
}

test_concurrent() {
    echo ""
    echo "== Concurrent requests =="
    seq 1 50 | xargs -P 10 -I{} curl -sf --max-time 5 "${BASE}/text" >/dev/null 2>&1 || true
    if curl -sf --max-time 5 "${BASE}/text" >/dev/null 2>&1; then
        pass "50 parallel requests (server still healthy)"
    else
        fail "Server unhealthy after parallel burst"
    fi
}

test_load() {
    echo ""
    echo "== Load test (1k conn, 10s) =="
    if ! command -v wrk >/dev/null 2>&1; then
        echo "  [SKIP] wrk not installed"
        return
    fi
    local out rps read_err
    out=$(wrk -t8 -c1000 -d10s --timeout 10s "${BASE}/text" 2>&1 || true)
    rps=$(echo "${out}" | awk '/Requests\/sec/ {print $2}')
    read_err=$(echo "${out}" | grep -o 'read [0-9]*' | awk '{print $2}' || echo 0)
    if [[ -n "${rps}" ]] && [[ "${read_err:-0}" -eq 0 ]]; then
        pass "wrk 1k: ${rps} req/s, 0 read errors"
    elif [[ -n "${rps}" ]]; then
        fail "wrk 1k: ${rps} req/s, read errors=${read_err}"
    else
        fail "wrk produced no output"
    fi
}

test_static_assets() {
    echo ""
    echo "== Static assets =="
    assert_status GET "/public/style.css" 200 "GET /public/style.css"
    assert_status GET "/public/script.js" 200 "GET /public/script.js"
}

trap stop_server EXIT

echo "============================================"
echo " WFX Integration Test Suite"
echo " Project: ${PROJECT}"
echo "============================================"

echo ""
echo "== Build =="
cd "${ROOT}/build" && cmake --build . -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null
pass "Engine build"

echo ""
echo "== Single worker =="
start_server 1
test_endpoints
test_static_assets
test_keepalive
test_concurrent
test_load
stop_server

echo ""
echo "== Multi worker (shared listen, 4 workers) =="
start_server 4
test_endpoints
test_keepalive
test_load
stop_server

echo ""
echo "============================================"
echo " Results: ${PASS} passed, ${FAIL} failed"
echo "============================================"
[[ ${FAIL} -eq 0 ]]
