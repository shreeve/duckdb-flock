#!/usr/bin/env bash
# Golden HTTP-roundtrip test for PR-5 /sql.
#
# Coverage:
#   - Auth: bearer, cookie, invalid token
#   - CORS preflight on /sql
#   - Default NDJSON row mode
#   - NDJSON chunk mode
#   - One-shot JSON mode
#   - Request validation: missing sql, multi-statement, __HARBOR_ADMIN__, oversize body
#   - Params: implicit `$1`, typed wrapper DECIMAL, typed NULL
#   - Sessions: create, transaction state across requests, delete, foreign/not-found 404
#   - /auth/logout?destroy_sessions=true destroys owned SQL sessions
#   - Representative type encodings: BIGINT string, DECIMAL string, INTERVAL object, BLOB base64, JSON text string
#
# Usage:
#   make release
#   scripts/golden-sql-roundtrip.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT_PATH="${REPO_ROOT}/build/release/extension/harbor/harbor.duckdb_extension"
DUCKDB_BIN="${REPO_ROOT}/build/release/duckdb"
PORT="${HARBOR_SQL_TEST_PORT:-19506}"
TOKEN="sql-golden-token-$$"
SERVER_LOG="$(mktemp)"
COOKIE_JAR="$(mktemp)"
SERVER_PID=""

cleanup() {
    if [[ -n "${SERVER_PID}" ]]; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -f "${SERVER_LOG}" "${COOKIE_JAR}" /tmp/harbor-sql-*.json /tmp/harbor-sql-*.ndjson /tmp/harbor-sql-big-body.json
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

nohup "${DUCKDB_BIN}" -unsigned -no-stdin -c "
LOAD '${EXT_PATH}';
SET GLOBAL harbor_cors_origins='https://app.example.com';
SET GLOBAL harbor_max_request_body_bytes=1024;
CALL harbor_serve('quack:127.0.0.1:${PORT}', token := '${TOKEN}');
CALL harbor_wait();
" > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!
sleep 2

# Ensure server is listening.
curl -sf -o /dev/null "http://127.0.0.1:${PORT}/info" || {
    echo "--- server log ---" >&2
    cat "${SERVER_LOG}" >&2
    fail "server did not start"
}
pass "server started"

# ---- CORS preflight ----
PREFLIGHT="$(curl -s -i -X OPTIONS "http://127.0.0.1:${PORT}/sql" \
    -H 'Origin: https://app.example.com' \
    -H 'Access-Control-Request-Method: POST' \
    -H 'Access-Control-Request-Headers: Authorization, Content-Type')"
echo "${PREFLIGHT}" | grep -qi '^HTTP/1.1 204' \
    || fail "OPTIONS /sql expected 204"
echo "${PREFLIGHT}" | grep -qi '^Access-Control-Allow-Origin: https://app.example.com' \
    || fail "OPTIONS /sql must echo allowed Origin"
echo "${PREFLIGHT}" | grep -qi '^Access-Control-Allow-Credentials: true' \
    || fail "OPTIONS /sql must include Allow-Credentials"
pass "OPTIONS /sql CORS preflight"

PREFLIGHT_SESS="$(curl -s -i -X OPTIONS "http://127.0.0.1:${PORT}/sql/sessions/new" \
    -H 'Origin: https://app.example.com' \
    -H 'Access-Control-Request-Method: POST' \
    -H 'Access-Control-Request-Headers: Authorization, Content-Type')"
echo "${PREFLIGHT_SESS}" | grep -qi '^HTTP/1.1 204' \
    || fail "OPTIONS /sql/sessions/new expected 204"
echo "${PREFLIGHT_SESS}" | grep -qi '^Access-Control-Allow-Methods: .*POST' \
    || fail "OPTIONS /sql/sessions/new must allow POST"
pass "OPTIONS /sql/sessions/new CORS preflight"

# ---- NDJSON row mode ----
curl -sfN -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT 42 AS answer, '\''hello'\'' AS greeting"}' \
    > /tmp/harbor-sql-row.ndjson
grep -q '"type":"schema"' /tmp/harbor-sql-row.ndjson || fail "row mode missing schema"
grep -q '"duckdbType":"INTEGER"' /tmp/harbor-sql-row.ndjson || fail "row mode missing INTEGER schema"
grep -q '"values":\[42,"hello"\]' /tmp/harbor-sql-row.ndjson || fail "row mode missing row values"
grep -q '"type":"end"' /tmp/harbor-sql-row.ndjson || fail "row mode missing end"
pass "POST /sql NDJSON row mode"

# ---- NDJSON chunk mode ----
curl -sfN -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Accept: application/x-ndjson; shape=chunk' \
    -H 'Content-Type: application/json' \
    -d '{"sql":"FROM range(3) SELECT range AS i"}' \
    > /tmp/harbor-sql-chunk.ndjson
grep -q '"type":"chunk"' /tmp/harbor-sql-chunk.ndjson || fail "chunk mode missing chunk line"
grep -q '"rows":\[\["0"\],\["1"\],\["2"\]\]' /tmp/harbor-sql-chunk.ndjson || fail "chunk mode rows mismatch"
pass "POST /sql NDJSON chunk mode"

# ---- One-shot JSON ----
curl -sf -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Accept: application/json' \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT 1 AS x, 2 AS y"}' \
    > /tmp/harbor-sql-oneshot.json
grep -q '"ok":true' /tmp/harbor-sql-oneshot.json || fail "one-shot missing ok"
grep -q '"kind":"select"' /tmp/harbor-sql-oneshot.json || fail "one-shot missing kind"
grep -q '"data":\[\[1,2\]\]' /tmp/harbor-sql-oneshot.json || fail "one-shot data mismatch"
pass "POST /sql one-shot JSON"

# ---- Validation failures ----
code="$(curl -s -o /tmp/harbor-sql-missing.json -w '%{http_code}' \
    -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -d '{}')"
[[ "${code}" == "400" ]] || fail "missing sql expected 400, got ${code}"
grep -q '"errorCode":"BAD_REQUEST"' /tmp/harbor-sql-missing.json || fail "missing sql error code"
pass "missing sql rejected"

code="$(curl -s -o /tmp/harbor-sql-multi.json -w '%{http_code}' \
    -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT 1; SELECT 2;"}')"
[[ "${code}" == "400" ]] || fail "multi-statement expected 400, got ${code}"
grep -q '"errorCode":"BAD_REQUEST"' /tmp/harbor-sql-multi.json || fail "multi-statement error code"
pass "multi-statement rejected"

code="$(curl -s -o /tmp/harbor-sql-admin.json -w '%{http_code}' \
    -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"__HARBOR_ADMIN__:checkpoint:create"}')"
[[ "${code}" == "400" ]] || fail "__HARBOR_ADMIN__ expected 400, got ${code}"
pass "__HARBOR_ADMIN__ reserved prefix rejected"

python3 - <<'PY' >/tmp/harbor-sql-big-body.json
print('{"sql":"' + ('x' * 2000) + '"}')
PY
code="$(curl -s -o /tmp/harbor-sql-big-response.json -w '%{http_code}' \
    -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    --data-binary @/tmp/harbor-sql-big-body.json)"
[[ "${code}" == "413" ]] || fail "oversized body expected 413, got ${code}"
pass "oversized body rejected"

# ---- Auth failures and cookie auth ----
code="$(curl -s -o /tmp/harbor-sql-auth.json -w '%{http_code}' \
    -X POST "http://127.0.0.1:${PORT}/sql" \
    -H 'Authorization: Bearer wrong-token' \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT 1"}')"
[[ "${code}" == "401" ]] || fail "invalid token expected 401, got ${code}"
pass "invalid bearer rejected"

LOGIN="$(curl -s -i -X POST "http://127.0.0.1:${PORT}/auth/login" \
    -H 'Content-Type: application/json' \
    -d "{\"token\":\"${TOKEN}\"}")"
COOKIE="$(echo "${LOGIN}" | grep -i '^Set-Cookie: harbor_session=' | sed -E 's/^[Ss]et-[Cc]ookie: harbor_session=([^;]+).*$/\1/' | tr -d '\r')"
[[ -n "${COOKIE}" ]] || fail "failed to obtain harbor_session cookie"
curl -sfN -X POST "http://127.0.0.1:${PORT}/sql" \
    -b "harbor_session=${COOKIE}" \
    -H 'Origin: https://app.example.com' \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT 7 AS cookie_ok"}' \
    > /tmp/harbor-sql-cookie.ndjson
grep -q '"values":\[7\]' /tmp/harbor-sql-cookie.ndjson || fail "cookie-auth /sql did not return row"
pass "cookie auth works for /sql"

# ---- Params ----
curl -sfN -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT $1::INTEGER + 1 AS n","params":[41]}' \
    > /tmp/harbor-sql-param.ndjson
grep -q '"values":\[42\]' /tmp/harbor-sql-param.ndjson || fail "implicit param failed"
pass "implicit params work"

curl -sfN -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT $1 AS d, $2 AS maybe_null","params":[{"value":"123.4567","type":"DECIMAL(18,4)"},{"value":null,"type":"INTEGER"}]}' \
    > /tmp/harbor-sql-typed-param.ndjson
grep -q '"duckdbType":"DECIMAL(18,4)"' /tmp/harbor-sql-typed-param.ndjson || fail "typed decimal schema missing"
grep -q '"values":\["123.4567",null\]' /tmp/harbor-sql-typed-param.ndjson || fail "typed param row mismatch"
pass "typed-wrapper params work"

# ---- Representative type encodings ----
curl -sfN -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT 9223372036854775807::BIGINT AS big, 12345.6789::DECIMAL(18,4) AS dec_val, INTERVAL 1 YEAR + INTERVAL 2 DAYS + INTERVAL 3 SECONDS AS iv, '\''hello world'\''::BLOB AS b, '\''{\"a\":1}'\''::JSON AS j"}' \
    > /tmp/harbor-sql-types.ndjson
grep -q '"9223372036854775807"' /tmp/harbor-sql-types.ndjson || fail "BIGINT should be string"
grep -q '"12345.6789"' /tmp/harbor-sql-types.ndjson || fail "DECIMAL should be string"
grep -q '"months":12,"days":2,"micros":"3000000"' /tmp/harbor-sql-types.ndjson || fail "INTERVAL object mismatch"
grep -q '"aGVsbG8gd29ybGQ="' /tmp/harbor-sql-types.ndjson || fail "BLOB base64 mismatch"
grep -q '"{\\"a\\":1}"' /tmp/harbor-sql-types.ndjson || fail "JSON column should be JSON-text string"
pass "representative type encodings"

# ---- Sessions + transaction state ----
SESS="$(curl -sf -X POST "http://127.0.0.1:${PORT}/sql/sessions/new" -H "Authorization: Bearer ${TOKEN}" -d '')"
SID="$(echo "${SESS}" | sed -E 's/.*"sessionId":"([^"]+)".*/\1/')"
[[ -n "${SID}" ]] || fail "session create returned no sessionId"

curl -sf -X POST "http://127.0.0.1:${PORT}/sql" -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' -d "{\"sql\":\"BEGIN\",\"sessionId\":\"${SID}\"}" >/dev/null
curl -sf -X POST "http://127.0.0.1:${PORT}/sql" -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' -d "{\"sql\":\"CREATE TEMP TABLE t (i INT)\",\"sessionId\":\"${SID}\"}" >/dev/null
curl -sf -X POST "http://127.0.0.1:${PORT}/sql" -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' -d "{\"sql\":\"INSERT INTO t VALUES (1),(2),(3)\",\"sessionId\":\"${SID}\"}" >/dev/null
curl -sfN -X POST "http://127.0.0.1:${PORT}/sql" -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' -d "{\"sql\":\"SELECT count(*) AS n FROM t\",\"sessionId\":\"${SID}\"}" \
    > /tmp/harbor-sql-session.ndjson
grep -q '"values":\["3"\]' /tmp/harbor-sql-session.ndjson || fail "session transaction state not visible"
pass "session transaction state survives requests"

DEL="$(curl -s -i -X DELETE "http://127.0.0.1:${PORT}/sql/sessions/${SID}" -H "Authorization: Bearer ${TOKEN}")"
echo "${DEL}" | grep -qi '^HTTP/1.1 200' || fail "session DELETE expected 200"
echo "${DEL}" | grep -q '"ok":true' || fail "session DELETE missing ok"
PREFLIGHT_DEL="$(curl -s -i -X OPTIONS "http://127.0.0.1:${PORT}/sql/sessions/${SID}" \
    -H 'Origin: https://app.example.com' \
    -H 'Access-Control-Request-Method: DELETE' \
    -H 'Access-Control-Request-Headers: Authorization')"
echo "${PREFLIGHT_DEL}" | grep -qi '^HTTP/1.1 204' \
    || fail "OPTIONS /sql/sessions/<id> expected 204"
echo "${PREFLIGHT_DEL}" | grep -qi '^Access-Control-Allow-Methods: .*DELETE' \
    || fail "OPTIONS /sql/sessions/<id> must allow DELETE"
pass "OPTIONS /sql/sessions/<id> CORS preflight"
code="$(curl -s -o /tmp/harbor-sql-session-gone.json -w '%{http_code}' \
    -X POST "http://127.0.0.1:${PORT}/sql" -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' -d "{\"sql\":\"SELECT 1\",\"sessionId\":\"${SID}\"}")"
[[ "${code}" == "404" ]] || fail "using deleted session expected 404, got ${code}"
pass "session delete + not-found behavior"

# ---- logout?destroy_sessions=true ----
SESS2="$(curl -sf -X POST "http://127.0.0.1:${PORT}/sql/sessions/new" -H "Authorization: Bearer ${TOKEN}" -d '')"
SID2="$(echo "${SESS2}" | sed -E 's/.*"sessionId":"([^"]+)".*/\1/')"
[[ -n "${SID2}" ]] || fail "second session create returned no sessionId"
curl -sf -X POST "http://127.0.0.1:${PORT}/auth/logout?destroy_sessions=true" \
    -b "harbor_session=${COOKIE}" -d '' >/tmp/harbor-sql-logout.json
code="$(curl -s -o /tmp/harbor-sql-session-destroyed.json -w '%{http_code}' \
    -X POST "http://127.0.0.1:${PORT}/sql" -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' -d "{\"sql\":\"SELECT 1\",\"sessionId\":\"${SID2}\"}")"
[[ "${code}" == "404" ]] || fail "logout destroyed session expected 404, got ${code}"
pass "logout destroy_sessions removes owned SQL sessions"

echo
green "All /sql golden assertions passed."
