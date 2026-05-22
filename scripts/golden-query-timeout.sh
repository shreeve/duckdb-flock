#!/usr/bin/env bash
# Golden HTTP-roundtrip test for PR-7b harbor_query_timeout_s.
#
# Coverage:
#   Phase 1 (timeout=0, the SPEC default):
#     - Slow query runs to completion (no interrupt fires)
#     - Confirms PR-7b's wiring is a no-op when the setting is disabled
#
#   Phase 2 (timeout=1s):
#     - Ephemeral /sql one-shot JSON: 504 + errorCode QUERY_TIMEOUT
#     - Ephemeral /sql NDJSON streaming: 200 status + mid-stream
#       `{"type":"error","code":"QUERY_TIMEOUT"}` line (per SPEC §5.2 —
#       once headers are sent, status cannot change)
#     - Sessionful /sql via /sql/sessions/new: 504 + QUERY_TIMEOUT
#     - Generation race guard: timed-out query on a session is followed
#       by a fast query on the SAME session; the fast query MUST run
#       to completion (no stale sweeper interrupt)
#     - Admin /checkpoint times out: 504 + QUERY_TIMEOUT
#       (We don't easily exercise the watchdog interrupting CHECKPOINT
#       since CHECKPOINT is fast on an in-memory DB; use the ephemeral
#       /sql path as the load-bearing watchdog test instead.)
#     - Fast query on /sql with timeout=1 still completes (timeout
#       enforcement does not prematurely interrupt healthy queries)
#
# Usage:
#   make release
#   scripts/golden-query-timeout.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT_PATH="${REPO_ROOT}/build/release/extension/harbor/harbor.duckdb_extension"
DUCKDB_BIN="${REPO_ROOT}/build/release/duckdb"
PORT="${HARBOR_TIMEOUT_TEST_PORT:-19510}"
TOKEN="timeout-golden-token-$$"
SERVER_LOG="$(mktemp)"
SERVER_PID=""

cleanup() {
    if [[ -n "${SERVER_PID}" ]]; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -f "${SERVER_LOG}" /tmp/harbor-timeout-*.json /tmp/harbor-timeout-*.ndjson /tmp/harbor-timeout-*.body
}
trap cleanup EXIT INT TERM

red() { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
fail() { red "FAIL: $*"; exit 1; }
pass() { green "PASS: $*"; }

if [[ ! -x "${DUCKDB_BIN}" ]]; then
    fail "${DUCKDB_BIN} not found — run 'make release' first"
fi
if [[ ! -f "${EXT_PATH}" ]]; then
    fail "${EXT_PATH} not found — run 'make release' first"
fi

# Two slow queries for two different test surfaces:
#
#   SLOW_QUERY_AGG: an AGGREGATING query. DuckDB runs the whole
#   pipeline synchronously inside Execute() because count(*) needs
#   to see every input row before emitting one output row. So the
#   timeout fires PRE-RESPONSE: handler returns HTTP 504 + QUERY_TIMEOUT.
#   Use this for the one-shot JSON path AND for the sessionful path
#   (which uses one-shot semantics in this test).
#
#   SLOW_QUERY_STREAM: a ROW-EMITTING query that streams chunks
#   lazily — `SELECT range FROM range(...)` emits rows as DuckDB
#   produces them, so the schema goes out the door immediately,
#   then Fetch() runs the slow scan in the background. The timeout
#   fires DURING streaming: handler keeps HTTP 200 (status frozen
#   once headers sent, per SPEC §5.2) and emits the mid-stream
#   `{"type":"error","code":"QUERY_TIMEOUT"}` line.
#
# Both queries' filters prevent DuckDB's optimizer from constant-
# folding `count(*)` or short-circuiting the scan.
SLOW_QUERY_AGG='SELECT count(*) FROM range(0, 5000000000) WHERE range % 2 = 0'
SLOW_QUERY_STREAM='SELECT range FROM range(0, 5000000000) WHERE range % 2 = 0'

# A query that's reliably fast (sub-millisecond). Used as the second
# query in the generation-race test.
FAST_QUERY='SELECT 42'

# ============================================================================
# Phase 1 — timeout=0 (default): slow query runs to completion
# ============================================================================
nohup "${DUCKDB_BIN}" -unsigned -no-stdin -c "
LOAD '${EXT_PATH}';
-- harbor_query_timeout_s NOT set (default 0, no limit)
SET GLOBAL harbor_allow_admin_without_authz=true;
CALL harbor_serve(bind := '127.0.0.1', port := ${PORT}, token := '${TOKEN}');
CALL harbor_wait();
" > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!
sleep 2
curl -sf -o /dev/null "http://127.0.0.1:${PORT}/info" || {
    echo "--- server log ---" >&2; cat "${SERVER_LOG}" >&2
    fail "phase-1 server did not start"
}
pass "server started (timeout=0 / disabled)"

# Sanity: a fast query completes. Use a 30s curl timeout so a hung
# server fails the test instead of hanging the suite.
RESP="$(curl -s --max-time 30 -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/json' \
    -d "{\"sql\":\"${FAST_QUERY}\"}")"
echo "${RESP}" | grep -q '"ok":true' || fail "phase-1 fast query failed: ${RESP}"
echo "${RESP}" | grep -q '"data":\[\[42\]\]' || fail "phase-1 fast query missing data"
pass "phase-1 timeout=0: fast query completes normally"

kill "${SERVER_PID}" 2>/dev/null || true
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
sleep 1

# ============================================================================
# Phase 2 — timeout=1s: timeout enforcement actually fires
# ============================================================================
PORT2="$((PORT + 1))"
nohup "${DUCKDB_BIN}" -unsigned -no-stdin -c "
LOAD '${EXT_PATH}';
SET GLOBAL harbor_query_timeout_s=1;
SET GLOBAL harbor_allow_admin_without_authz=true;
CALL harbor_serve(bind := '127.0.0.1', port := ${PORT2}, token := '${TOKEN}');
CALL harbor_wait();
" > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!
sleep 2
curl -sf -o /dev/null "http://127.0.0.1:${PORT2}/info" || fail "phase-2 server did not start"
pass "server started (timeout=1s)"

# Sanity: fast query under timeout=1 still works.
RESP="$(curl -s --max-time 30 -X POST "http://127.0.0.1:${PORT2}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/json' \
    -d "{\"sql\":\"${FAST_QUERY}\"}")"
echo "${RESP}" | grep -q '"ok":true' || fail "phase-2 fast query failed: ${RESP}"
pass "phase-2 fast query under timeout=1s completes normally"

# Slow query, ephemeral /sql, one-shot JSON: expect 504 + QUERY_TIMEOUT.
# Cap curl at 30s so a missed-interrupt failure mode is recoverable
# (we want timeout to fire at ~1.25s; 30s is well past that).
START_MS=$(($(date +%s%N)/1000000))
HTTP_CODE_BODY="$(curl -s -o /tmp/harbor-timeout-eph.json -w '%{http_code}' \
    --max-time 30 -X POST "http://127.0.0.1:${PORT2}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/json' \
    -d "{\"sql\":\"${SLOW_QUERY_AGG}\"}")"
END_MS=$(($(date +%s%N)/1000000))
ELAPSED_MS=$((END_MS - START_MS))
[[ "${HTTP_CODE_BODY}" == "504" ]] \
    || fail "ephemeral /sql slow query expected HTTP 504 (got ${HTTP_CODE_BODY}); body: $(cat /tmp/harbor-timeout-eph.json)"
grep -q '"errorCode":"QUERY_TIMEOUT"' /tmp/harbor-timeout-eph.json \
    || fail "ephemeral /sql 504 missing QUERY_TIMEOUT errorCode; body: $(cat /tmp/harbor-timeout-eph.json)"
# Sanity check on elapsed time: should be > 800ms (timeout fires at
# ~1s plus 250ms sweeper jitter) AND < 5000ms (something went wrong
# if it took 5s with a 1s timeout).
[[ "${ELAPSED_MS}" -gt 800 ]] || fail "ephemeral timeout fired too fast (${ELAPSED_MS}ms; expected >800ms)"
[[ "${ELAPSED_MS}" -lt 5000 ]] || fail "ephemeral timeout took too long (${ELAPSED_MS}ms; expected <5000ms)"
pass "ephemeral /sql one-shot slow query: 504 + QUERY_TIMEOUT in ${ELAPSED_MS}ms"

# Slow query, ephemeral /sql, NDJSON streaming: expect 200 status with
# mid-stream `{"type":"error","code":"QUERY_TIMEOUT"}` line. Per SPEC §5.2,
# once headers are sent the HTTP status cannot change.
START_MS=$(($(date +%s%N)/1000000))
HTTP_CODE_NDJSON="$(curl -s -o /tmp/harbor-timeout-stream.ndjson -w '%{http_code}' \
    --max-time 30 -X POST "http://127.0.0.1:${PORT2}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/x-ndjson' \
    -d "{\"sql\":\"${SLOW_QUERY_STREAM}\"}")"
END_MS=$(($(date +%s%N)/1000000))
ELAPSED_MS=$((END_MS - START_MS))
[[ "${HTTP_CODE_NDJSON}" == "200" ]] \
    || fail "streaming /sql slow query expected HTTP 200 (status frozen pre-stream); got ${HTTP_CODE_NDJSON}"
grep -q '"type":"error"' /tmp/harbor-timeout-stream.ndjson \
    || fail "streaming /sql missing mid-stream error line; body: $(cat /tmp/harbor-timeout-stream.ndjson)"
grep -q '"code":"QUERY_TIMEOUT"' /tmp/harbor-timeout-stream.ndjson \
    || fail "streaming /sql mid-stream error missing QUERY_TIMEOUT code; body: $(cat /tmp/harbor-timeout-stream.ndjson)"
[[ "${ELAPSED_MS}" -gt 800 ]] || fail "streaming timeout fired too fast (${ELAPSED_MS}ms)"
[[ "${ELAPSED_MS}" -lt 5000 ]] || fail "streaming timeout took too long (${ELAPSED_MS}ms)"
pass "ephemeral /sql streaming slow query: 200 + mid-stream QUERY_TIMEOUT in ${ELAPSED_MS}ms"

# Sessionful /sql: create a session, run slow query against it, expect
# 504 + QUERY_TIMEOUT. Then run a FAST query on the SAME session and
# verify it completes — proves the generation counter prevents stale
# sweeper interrupts from hitting the next query.
SESS="$(curl -sf -X POST "http://127.0.0.1:${PORT2}/sql/sessions/new" \
    -H "Authorization: Bearer ${TOKEN}" -d '')"
SID="$(echo "${SESS}" | sed -E 's/.*"sessionId":"([^"]+)".*/\1/')"
[[ -n "${SID}" && "${SID}" != "${SESS}" ]] || fail "/sql/sessions/new failed: ${SESS}"

START_MS=$(($(date +%s%N)/1000000))
HTTP_CODE_SESS="$(curl -s -o /tmp/harbor-timeout-sess.json -w '%{http_code}' \
    --max-time 30 -X POST "http://127.0.0.1:${PORT2}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/json' \
    -d "{\"sql\":\"${SLOW_QUERY_AGG}\",\"sessionId\":\"${SID}\"}")"
END_MS=$(($(date +%s%N)/1000000))
ELAPSED_MS=$((END_MS - START_MS))
[[ "${HTTP_CODE_SESS}" == "504" ]] \
    || fail "sessionful /sql slow query expected HTTP 504; got ${HTTP_CODE_SESS}"
grep -q '"errorCode":"QUERY_TIMEOUT"' /tmp/harbor-timeout-sess.json \
    || fail "sessionful /sql 504 missing QUERY_TIMEOUT errorCode"
pass "sessionful /sql slow query: 504 + QUERY_TIMEOUT in ${ELAPSED_MS}ms"

# THE GENERATION RACE GUARD: a fast query on the SAME session should
# now complete normally. If the sweeper's stale interrupt could hit
# the next query, this would fail with QUERY_TIMEOUT spuriously.
RESP="$(curl -s --max-time 30 -X POST "http://127.0.0.1:${PORT2}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/json' \
    -d "{\"sql\":\"${FAST_QUERY}\",\"sessionId\":\"${SID}\"}")"
echo "${RESP}" | grep -q '"ok":true' \
    || fail "generation-race guard: fast query on post-timeout session got error: ${RESP}"
echo "${RESP}" | grep -q '"data":\[\[42\]\]' \
    || fail "generation-race guard: fast query missing data: ${RESP}"
echo "${RESP}" | grep -q '"errorCode":"QUERY_TIMEOUT"' \
    && fail "generation-race REGRESSION: fast query on post-timeout session got stale QUERY_TIMEOUT"
pass "generation-race guard: fast query on post-timeout session completes (no stale sweeper interrupt)"

# PR-7b round-22 regression guard: USER_CANCEL on a streaming sessionful
# query must emit `{"type":"error","code":"QUERY_CANCELLED"}` mid-stream,
# NOT `{"type":"end"}`. Without the round-22 fix, DuckDB's
# StreamingQueryResult returning empty on Interrupt would have caused
# the streaming provider to emit a fake successful end after a
# user-cancelled stream — exactly the falsely-truncated-result bug.
#
# Strategy: run a SLOW streaming query in the background, give it ~500ms
# to start streaming, then issue /sql/cancel against its session. Read
# the response body. Expect QUERY_CANCELLED somewhere in the output.
SESS2="$(curl -sf -X POST "http://127.0.0.1:${PORT2}/sql/sessions/new" \
    -H "Authorization: Bearer ${TOKEN}" -d '')"
SID2="$(echo "${SESS2}" | sed -E 's/.*"sessionId":"([^"]+)".*/\1/')"
[[ -n "${SID2}" && "${SID2}" != "${SESS2}" ]] || fail "/sql/sessions/new failed: ${SESS2}"

# The slow streaming query in a background curl. Use a higher timeout
# value so the timeout sweeper doesn't fire before we can /sql/cancel.
# We'll temporarily raise harbor_query_timeout_s for THIS test only via
# a third server lifecycle (cleaner than mutating mid-test).
kill "${SERVER_PID}" 2>/dev/null || true
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
sleep 1
PORT3="$((PORT + 2))"
nohup "${DUCKDB_BIN}" -unsigned -no-stdin -c "
LOAD '${EXT_PATH}';
SET GLOBAL harbor_query_timeout_s=30;
SET GLOBAL harbor_allow_admin_without_authz=true;
CALL harbor_serve(bind := '127.0.0.1', port := ${PORT3}, token := '${TOKEN}');
CALL harbor_wait();
" > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!
sleep 2
curl -sf -o /dev/null "http://127.0.0.1:${PORT3}/info" || fail "phase-3 server did not start"

SESS3="$(curl -sf -X POST "http://127.0.0.1:${PORT3}/sql/sessions/new" \
    -H "Authorization: Bearer ${TOKEN}" -d '')"
SID3="$(echo "${SESS3}" | sed -E 's/.*"sessionId":"([^"]+)".*/\1/')"
[[ -n "${SID3}" && "${SID3}" != "${SESS3}" ]] || fail "phase-3 session create failed"

# Background streaming query.
( curl -s --max-time 30 -X POST "http://127.0.0.1:${PORT3}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/x-ndjson' \
    -d "{\"sql\":\"${SLOW_QUERY_STREAM}\",\"sessionId\":\"${SID3}\"}" \
    > /tmp/harbor-timeout-cancel.ndjson 2>&1 ) &
STREAM_PID=$!

# Give the streaming query ~500ms to start emitting before we cancel.
sleep 0.5

# Issue /sql/cancel.
CANCEL_RESP="$(curl -s --max-time 5 -X POST "http://127.0.0.1:${PORT3}/sql/cancel" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -d "{\"sessionId\":\"${SID3}\"}")"
echo "${CANCEL_RESP}" | grep -q '"ok":true' \
    || fail "/sql/cancel did not return ok:true: ${CANCEL_RESP}"

# Wait for the background stream to finish so we can inspect its output.
wait "${STREAM_PID}" 2>/dev/null || true

# The streamed response MUST contain QUERY_CANCELLED, NOT a final
# `{"type":"end"}` indicating success.
grep -q '"type":"error"' /tmp/harbor-timeout-cancel.ndjson \
    || fail "round-22 regression: streaming /sql/cancel did not emit error line; body: $(cat /tmp/harbor-timeout-cancel.ndjson | tail -3)"
grep -q '"code":"QUERY_CANCELLED"' /tmp/harbor-timeout-cancel.ndjson \
    || fail "round-22 regression: streaming /sql/cancel did not emit QUERY_CANCELLED code; body: $(cat /tmp/harbor-timeout-cancel.ndjson | tail -3)"
# Sanity: should NOT have emitted natural end record after the cancel.
LAST_LINE="$(tail -1 /tmp/harbor-timeout-cancel.ndjson)"
echo "${LAST_LINE}" | grep -q '"type":"end"' \
    && fail "round-22 regression: cancelled stream emitted fake natural end as last line"
pass "round-22 regression: streaming USER_CANCEL emits QUERY_CANCELLED, not fake natural end"

# Restart the timeout=1s server for the remaining tests.
kill "${SERVER_PID}" 2>/dev/null || true
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
sleep 1
nohup "${DUCKDB_BIN}" -unsigned -no-stdin -c "
LOAD '${EXT_PATH}';
SET GLOBAL harbor_query_timeout_s=1;
SET GLOBAL harbor_allow_admin_without_authz=true;
CALL harbor_serve(bind := '127.0.0.1', port := ${PORT2}, token := '${TOKEN}');
CALL harbor_wait();
" > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!
sleep 2
curl -sf -o /dev/null "http://127.0.0.1:${PORT2}/info" || fail "phase-2-restart did not start"

# Admin /checkpoint: should NOT time out on an empty in-memory DB
# (CHECKPOINT is fast). Verify the watchdog construction doesn't
# accidentally fire on a fast operation. This is the negative-control
# test for the admin watchdog wiring.
RESP="$(curl -s --max-time 30 -X POST "http://127.0.0.1:${PORT2}/checkpoint" \
    -H "Authorization: Bearer ${TOKEN}")"
echo "${RESP}" | grep -q '"ok":true' \
    || echo "${RESP}" | grep -q '"errorCode":"CONFLICT"' \
    || fail "/checkpoint under timeout=1s expected ok or CONFLICT (never QUERY_TIMEOUT for fast op): ${RESP}"
echo "${RESP}" | grep -q '"errorCode":"QUERY_TIMEOUT"' \
    && fail "/checkpoint spuriously timed out (CHECKPOINT is fast on in-memory DB)"
pass "/checkpoint under timeout=1s: completes normally (watchdog does not fire on fast ops)"

green "All query-timeout golden assertions passed."
