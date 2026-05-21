#!/usr/bin/env bash
# Golden regression test for harbor_local_dev_mode + EventSource same-origin
# /localEvents (the bug shipped in v0.1.2 surfaced in interactive UI testing
# 2026-05-21).
#
# Background: per the Fetch spec, browsers do NOT send Origin on same-origin
# "no-cors" requests like `new EventSource('/localEvents')`. v0.1.1 fixed
# the /localEvents pre-auth Origin gate to accept empty Origin as
# implicitly-same-origin (auth-gated below), BUT the local-dev bypass
# inside AuthorizeUiRequest still required Origin to be in the allow-list,
# so EventSource same-origin SSE in local_dev_mode silently 401'd and the
# DuckDB UI showed "Connection to DuckDB Lost" on every page load.
#
# This test pins the fix: with harbor_local_dev_mode = true and the server
# bound to loopback, /localEvents WITHOUT an Origin header MUST return
# 200 (text/event-stream). Cross-origin requests with a disallowed Origin
# MUST still be rejected pre-auth, and /ddb/run WITHOUT an Origin must
# still 401 at its pre-auth gate (the fix is scoped to /localEvents'
# empty-Origin path; it does NOT relax /ddb/run's stricter gate).
#
# Usage:
#   make release
#   scripts/golden-localdev-sse.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT_PATH="${REPO_ROOT}/build/release/extension/harbor/harbor.duckdb_extension"
DUCKDB_BIN="${REPO_ROOT}/build/release/duckdb"
PORT="${HARBOR_TEST_PORT:-19498}"
TOKEN="localdev-sse-golden-$$"
SERVER_LOG="$(mktemp)"
SERVER_PID=""

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
fail()  { red   "FAIL: $*"; exit 1; }
pass()  { green "PASS: $*"; }

if [[ ! -x "${DUCKDB_BIN}" ]]; then
    echo "FATAL: ${DUCKDB_BIN} not found — run 'make release' first." >&2
    exit 1
fi
if [[ ! -f "${EXT_PATH}" ]]; then
    echo "FATAL: ${EXT_PATH} not found — run 'make release' first." >&2
    exit 1
fi

cleanup() {
    if [[ -n "${SERVER_PID}" ]]; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -f "${SERVER_LOG}"
}
trap cleanup EXIT INT TERM

# Start harbor server with local_dev_mode = true, bound to loopback.
nohup "${DUCKDB_BIN}" -unsigned -no-stdin -c "
LOAD '${EXT_PATH}';
SET GLOBAL harbor_local_dev_mode = true;
CALL harbor_serve('quack:127.0.0.1:${PORT}', token := '${TOKEN}');
CALL harbor_wait();
" > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!
sleep 2

if ! curl -fsS "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
    echo "--- server log ---" >&2
    cat "${SERVER_LOG}" >&2
    fail "harbor failed to start on port ${PORT}"
fi

# ----------------------------------------------------------------------------
# A. /localEvents with NO Origin header (browser EventSource same-origin)
#    → must return 200 + text/event-stream. This is the regression assertion
#    that catches the v0.1.2 bug.
# ----------------------------------------------------------------------------
SSE_HEADERS="$(curl -sI -m 1 "http://127.0.0.1:${PORT}/localEvents" || true)"
if ! echo "${SSE_HEADERS}" | grep -qi '^HTTP/1.1 200'; then
    echo "--- response headers ---" >&2
    echo "${SSE_HEADERS}" >&2
    fail "/localEvents with no Origin must return 200 in local_dev_mode (regression: v0.1.2 returned 401, breaking the DuckDB UI's catalog-events SSE long-poll)"
fi
echo "${SSE_HEADERS}" | grep -qi '^Content-Type: text/event-stream' \
    || fail "/localEvents must respond with Content-Type: text/event-stream"
pass "A: /localEvents no-Origin (EventSource same-origin) → 200 + text/event-stream in local_dev_mode"

# ----------------------------------------------------------------------------
# B. /localEvents with a non-allow-listed Origin → must still 401.
#    Confirms the fix didn't blanket-accept all empty-or-cross-origin requests.
# ----------------------------------------------------------------------------
DISALLOWED="$(curl -sI -m 1 -H 'Origin: http://evil.example.com' "http://127.0.0.1:${PORT}/localEvents" || true)"
echo "${DISALLOWED}" | grep -qi '^HTTP/1.1 401' \
    || fail "/localEvents with disallowed Origin must still be 401 (CSRF defense)"
pass "B: /localEvents disallowed-Origin → 401 (CSRF defense intact)"

# ----------------------------------------------------------------------------
# C. /localEvents with an allowed loopback Origin → 200 (control case).
# ----------------------------------------------------------------------------
ALLOWED="$(curl -sI -m 1 -H "Origin: http://127.0.0.1:${PORT}" "http://127.0.0.1:${PORT}/localEvents" || true)"
echo "${ALLOWED}" | grep -qi '^HTTP/1.1 200' \
    || fail "/localEvents with allowed Origin must return 200"
pass "C: /localEvents allowed-Origin → 200 (control case)"

# ----------------------------------------------------------------------------
# D. /ddb/run with NO Origin → must still 401.
#    The fix is scoped to /localEvents' empty-Origin path. /ddb/run's
#    pre-auth Origin gate is stricter (browser XHR/fetch always sends
#    Origin) and an empty Origin there must remain pre-auth-rejected.
#    Note: must use a real POST (curl -X POST + --data-binary, NOT -sI
#    which forces HEAD and would silently use a different code path).
# ----------------------------------------------------------------------------
DDB_NO_ORIGIN_CODE="$(curl -s -o /dev/null -w "%{http_code}" -m 1 \
    -X POST -H 'Content-Type: application/octet-stream' \
    --data-binary 'x' "http://127.0.0.1:${PORT}/ddb/run" || true)"
[[ "${DDB_NO_ORIGIN_CODE}" == "401" ]] \
    || fail "/ddb/run with no Origin must still be 401 (pre-auth gate; the local-dev fix must NOT relax this). Got: ${DDB_NO_ORIGIN_CODE}"
pass "D: /ddb/run no-Origin → 401 (pre-auth gate intact; fix scoped to /localEvents)"

# ----------------------------------------------------------------------------
# E. /ddb/run with allowed Origin in local_dev_mode → must NOT be 401.
#    (It may be 400/500 from the binary parser since we send junk bytes,
#    but the AUTH gate must let it through.)
# ----------------------------------------------------------------------------
DDB_OK_CODE="$(curl -s -o /dev/null -w "%{http_code}" -m 2 \
    -X POST -H "Origin: http://127.0.0.1:${PORT}" \
    -H 'Content-Type: application/octet-stream' \
    --data-binary 'x' "http://127.0.0.1:${PORT}/ddb/run" || true)"
if [[ "${DDB_OK_CODE}" == "401" ]]; then
    fail "/ddb/run with allowed Origin in local_dev_mode must NOT 401 (local-dev bypass should grant access). Got: ${DDB_OK_CODE}"
fi
pass "E: /ddb/run allowed-Origin in local_dev_mode → not 401 (got ${DDB_OK_CODE}; local-dev bypass active)"

green "All harbor_local_dev_mode SSE regression assertions passed."
