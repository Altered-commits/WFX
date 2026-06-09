#!/usr/bin/env bash
# Full feature-route test: curl correctness + wrk @ 10k connections per route (macOS)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="${WFX_PROJECT:-wfx_loadtest}"
WFX="${ROOT}/wfx"
PORT=8080
BASE="http://127.0.0.1:${PORT}"
PASS=0
FAIL=0
SERVER_PID=""

# 10k benchmark defaults (override via env if needed)
WRK_THREADS="${WRK_THREADS:-4}"
WRK_CONNECTIONS="${WRK_CONNECTIONS:-10000}"
WRK_DURATION="${WRK_DURATION:-20s}"
WRK_TIMEOUT="${WRK_TIMEOUT:-10s}"
WRK_COOLDOWN="${WRK_COOLDOWN:-3}"   # seconds between 10k runs (server restarted each route)

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

stop_server() {
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    SERVER_PID=""
    pkill -f "wfx run ${PROJECT}" 2>/dev/null || true
    killall wfx 2>/dev/null || true
    sleep 2
}

wait_for_server() {
    for _ in $(seq 1 80); do
        curl -sf "${BASE}/text" >/dev/null 2>&1 && return 0
        sleep 0.25
    done
    return 1
}

start_server() {
    stop_server
    ulimit -n 65536 2>/dev/null || true
    cd "${ROOT}"
    "${WFX}" build "${PROJECT}" source >/dev/null
    "${WFX}" run "${PROJECT}" >/dev/null 2>&1 &
    SERVER_PID=$!
    if ! wait_for_server; then
        fail "Server failed to start"
        return 1
    fi
}

assert_status() {
    local method=$1 path=$2 expected=$3 label=$4
    shift 4
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -X "${method}" "$@" "${BASE}${path}" 2>/dev/null || echo "000")
    if [[ "${code}" == "${expected}" ]]; then
        pass "${label} (${code})"
    else
        fail "${label} (expected ${expected}, got ${code})"
    fi
}

assert_body_contains() {
    local path=$1 needle=$2 label=$3
    shift 3
    local body
    body=$(curl -sf --max-time 5 "$@" "${BASE}${path}" 2>/dev/null || echo "")
    if echo "${body}" | grep -q "${needle}"; then
        pass "${label}"
    else
        fail "${label} (missing '${needle}')"
    fi
}

parse_wrk() {
    local out=$1
    WRK_RPS=$(echo "${out}" | awk '/Requests\/sec/ {print $2}')
    WRK_READ_ERR=$(echo "${out}" | grep -oE 'read [0-9]+' | awk '{print $2}' | tail -1 || true)
    WRK_NON2XX=$(echo "${out}" | grep -oE 'Non-2xx or 3xx responses: [0-9]+' | awk '{print $5}' | tail -1 || true)
    WRK_READ_ERR=${WRK_READ_ERR:-0}
    WRK_NON2XX=${WRK_NON2XX:-0}
}

judge_wrk() {
    local label=$1
    if [[ -n "${WRK_RPS}" && "${WRK_RPS}" != "0.00" && "${WRK_READ_ERR}" -eq 0 && "${WRK_NON2XX}" -eq 0 ]]; then
        pass "${label}: ${WRK_RPS} req/s (${WRK_CONNECTIONS} conn, 0 read err, 0 non-2xx)"
    elif [[ -n "${WRK_RPS}" && "${WRK_READ_ERR}" -eq 0 && "${WRK_NON2XX}" -eq 0 ]]; then
        fail "${label}: ${WRK_RPS} req/s (no throughput)"
    elif [[ -n "${WRK_RPS}" && "${WRK_READ_ERR}" -eq 0 ]]; then
        fail "${label}: ${WRK_RPS} req/s, non-2xx=${WRK_NON2XX}"
    elif [[ -n "${WRK_RPS}" ]]; then
        fail "${label}: ${WRK_RPS} req/s, read errors=${WRK_READ_ERR}, non-2xx=${WRK_NON2XX}"
    else
        fail "${label}: no wrk output"
    fi
}

run_wrk_get() {
    local route=$1
    local url="${BASE}${route}"
    local extra=()
    if [[ "${route}" == "/secure" || "${route}" == "/secure-async" ]]; then
        extra=(-H "X-Test-Token: secret")
    fi

    start_server >/dev/null 2>&1 || return

    local out
    if ((${#extra[@]})); then
        out=$(wrk -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${WRK_DURATION}" --timeout "${WRK_TIMEOUT}" \
            "${extra[@]}" "${url}" 2>&1 || true)
    else
        out=$(wrk -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${WRK_DURATION}" --timeout "${WRK_TIMEOUT}" \
            "${url}" 2>&1 || true)
    fi

    parse_wrk "${out}"
    judge_wrk "wrk GET ${route}"

    stop_server
    sleep "${WRK_COOLDOWN}"
}

run_wrk_post() {
    local path=$1 method=$2 hdr=$3 body=$4

    start_server >/dev/null 2>&1 || return

    local out
    out=$(wrk -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${WRK_DURATION}" --timeout "${WRK_TIMEOUT}" \
        -s "${ROOT}/scripts/wrk_post.lua" "${BASE}${path}" -- "${method}" "${hdr}" "${body}" 2>&1 || true)

    parse_wrk "${out}"
    judge_wrk "wrk ${method} ${path}"

    stop_server
    sleep "${WRK_COOLDOWN}"
}

test_curl_routes() {
    echo ""
    echo "== Curl: basic responses =="
    assert_body_contains "/text" "Hello from WFX" "GET /text"
    assert_body_contains "/im-json" "WFX" "GET /im-json"
    assert_body_contains "/rm-json" "WEIRD" "GET /rm-json"
    assert_body_contains "/template" "Hello from WFX Template" "GET /template (SendFile compiled)"
    assert_body_contains "/template-live" "Hello from WFX Template" "GET /template-live (SendTemplate)"
    assert_body_contains "/static-file" "font-family" "GET /static-file (SendFile)"
    assert_body_contains "/write-manual" "manual body" "GET /write-manual"
    assert_body_contains "/stream" "chunk1chunk2chunk3" "GET /stream"
    assert_body_contains "/stream-live" "chunk1chunk2chunk3" "GET /stream-live (close-after-stream)"
    assert_body_contains "/async" "async ok" "GET /async"
    assert_body_contains "/endpoint" "endpoint registered" "GET /endpoint"
    assert_body_contains "/api/health" "OK" "GET /api/health (group)"
    assert_body_contains "/api/version" "version" "GET /api/version (group)"
    assert_body_contains "/users/42/posts/100" "user=42 post=100" "GET dynamic segments"
    assert_body_contains "/hello/world" "world" "GET string param"
    assert_body_contains "/files/docs/readme.txt" "path=docs/readme.txt" "GET wildcard"
    assert_body_contains "/metrics" "requests" "GET /metrics"
    assert_body_contains "/headers" "has_ua" "GET /headers"
    assert_body_contains "/form-page" "username" "GET /form-page"

    echo ""
    echo "== Curl: middleware =="
    assert_status GET "/secure" 401 "GET /secure without token"
    assert_body_contains "/secure" "secure ok" "GET /secure with token" -H "X-Test-Token: secret"
    assert_body_contains "/secure-async" "secure-async ok" "GET /secure-async" -H "X-Test-Token: secret"

    echo ""
    echo "== Curl: POST =="
    assert_body_contains "/json-echo" "echo" "POST /json-echo" \
        -H "Content-Type: application/json" \
        -d '{"name":"wfx"}'
    assert_body_contains "/form-submit" "ok" "POST /form-submit" \
        -H "Content-Type: application/x-www-form-urlencoded" \
        -d "username=alice&password=secret123"

    echo ""
    echo "== Curl: engine static + 404 =="
    assert_status GET "/public/style.css" 200 "GET /public/style.css"
    assert_status GET "/not-found" 404 "GET /not-found"
}

test_wrk_routes() {
    echo ""

    if ! command -v wrk >/dev/null 2>&1; then
        echo "  [SKIP] wrk not installed"
        return
    fi

    echo "== wrk: per-route @ ${WRK_CONNECTIONS} conn, ${WRK_DURATION}, -t${WRK_THREADS} =="
    echo "     (fresh server restart before each route)"

    local routes=(
        "/text"
        "/im-json"
        "/rm-json"
        "/template"
        "/template-live"
        "/static-file"
        "/write-manual"
        "/stream"
        "/async"
        "/endpoint"
        "/api/health"
        "/api/version"
        "/users/42/posts/100"
        "/hello/world"
        "/files/a/b/c"
        "/metrics"
        "/headers"
        "/secure"
        "/secure-async"
    )

    for route in "${routes[@]}"; do
        run_wrk_get "${route}"
    done

    echo ""
    echo "== wrk: POST routes @ ${WRK_CONNECTIONS} conn =="
    run_wrk_post "/json-echo" "POST" "Content-Type: application/json" '{"name":"wfx"}'
    run_wrk_post "/form-submit" "POST" "Content-Type: application/x-www-form-urlencoded" \
        "username=alice&password=secret123"

    echo ""
    echo "== wrk: mixed route rotation @ ${WRK_CONNECTIONS} conn =="
    start_server >/dev/null 2>&1 || return
    local out
    out=$(wrk -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${WRK_DURATION}" --timeout "${WRK_TIMEOUT}" \
        -s "${ROOT}/scripts/wrk_mixed.lua" "${BASE}/" 2>&1 || true)
    parse_wrk "${out}"
    judge_wrk "wrk mixed"
    stop_server
}

trap stop_server EXIT

echo "============================================"
echo " WFX Full Route Feature Test (10k conn)"
echo " Project: ${PROJECT}"
echo " wrk: -t${WRK_THREADS} -c${WRK_CONNECTIONS} -d${WRK_DURATION}"
echo "============================================"

echo ""
echo "== Engine build =="
cd "${ROOT}/build" && cmake --build . -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null
pass "Engine build"

start_server
test_curl_routes
stop_server

test_wrk_routes

echo ""
echo "============================================"
echo " Results: ${PASS} passed, ${FAIL} failed"
echo "============================================"
[[ ${FAIL} -eq 0 ]]
