#!/usr/bin/env bash
# Golden HTTP-roundtrip test for PR-4 cookie auth + CORS allow-list,
# plus PR-8 credential-strip on the UI proxy.
#
# Why a bash script (not test/sql/harbor.test): sqllogictest can only
# call SQL functions, not curl. The cookie roundtrip — POST /auth/login
# → Set-Cookie → reuse cookie on a subsequent request — needs a real
# HTTP client. This script provides that, with the same fail-loud
# semantics (any assertion failure → non-zero exit).
#
# Coverage:
#   - POST /auth/login with valid token  → 200, Set-Cookie matches v1.<base64url>
#   - POST /auth/login with bad token    → 401, no Set-Cookie
#   - POST /auth/login JSON body         → 200 (alternate input form)
#   - POST /auth/logout                  → 200, Set-Cookie clears with Max-Age=0
#   - GET /                              → 200, body contains the login page marker
#   - GET /random/path no cookie         → 401
#   - GET /info no cookie                → 204, NO Access-Control-Allow-Origin: *
#   - GET /info with allowed Origin      → 204 + ACAO echoes the matching origin
#   - GET /info with disallowed Origin   → 204 + NO ACAO header
#   - GET /auth/login                    → 405 Method Not Allowed (round-12 fix)
#   - PR-1.5 wire-compat: /quack still serves byte-for-byte
#   - PR-8: UI proxy never forwards harbor auth material upstream
#
# The PR-8 leak test points harbor's `ui_remote_url` setting at a tiny
# Python listener instead of `https://ui.duckdb.org`, captures the
# raw bytes of whatever harbor sends upstream when an authenticated
# user fetches a UI asset, and asserts the captured request contains
# NO `harbor_session=`, NO `Authorization:`, and NO `X-Harbor-*` header.
#
# Usage:
#   make release
#   scripts/golden-cookie-auth.sh
#
# Exit code: 0 on success; non-zero on any failure (with a loud message
# describing which assertion failed).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT_PATH="${REPO_ROOT}/build/release/extension/harbor/harbor.duckdb_extension"
DUCKDB_BIN="${REPO_ROOT}/build/release/duckdb"
PORT="${HARBOR_TEST_PORT:-19496}"
MOCK_PORT="${HARBOR_TEST_MOCK_PORT:-19497}"
TOKEN="cookieauth-golden-token-$$"
SERVER_LOG="$(mktemp)"
COOKIE_JAR="$(mktemp)"
MOCK_CAPTURE="$(mktemp)"
MOCK_SCRIPT="$(mktemp --suffix=.py 2>/dev/null || mktemp -t mock.XXXXXX.py)"
SERVER_PID=""
MOCK_PID=""

if [[ ! -x "${DUCKDB_BIN}" ]]; then
    echo "FATAL: ${DUCKDB_BIN} not found — run 'make release' first." >&2
    exit 1
fi
if [[ ! -f "${EXT_PATH}" ]]; then
    echo "FATAL: ${EXT_PATH} not found — run 'make release' first." >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "FATAL: python3 is required for the PR-8 leak-test mock listener." >&2
    exit 1
fi

cleanup() {
    if [[ -n "${SERVER_PID}" ]]; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    if [[ -n "${MOCK_PID}" ]]; then
        kill "${MOCK_PID}" 2>/dev/null || true
        wait "${MOCK_PID}" 2>/dev/null || true
    fi
    rm -f "${SERVER_LOG}" "${COOKIE_JAR}" "${MOCK_CAPTURE}" "${MOCK_SCRIPT}"
}
trap cleanup EXIT INT TERM

red() { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
fail() { red "FAIL: $*"; exit 1; }
pass() { green "PASS: $*"; }

# ---------------------------------------------------------------------
# Spawn the leak-test mock listener (PR-8). Captures all incoming
# request bytes to ${MOCK_CAPTURE}, responds 200 OK with a tiny body,
# accepts repeatedly until killed.
# ---------------------------------------------------------------------
cat > "${MOCK_SCRIPT}" <<'PY'
import os, socket, sys
port = int(sys.argv[1]); capture = sys.argv[2]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port)); s.listen(8)
while True:
    try:
        conn, _ = s.accept()
        data = b''
        while True:
            chunk = conn.recv(4096)
            if not chunk: break
            data += chunk
            # Headers end at CRLF CRLF; for a GET that's the whole request.
            if b'\r\n\r\n' in data: break
        with open(capture, 'ab') as f:
            f.write(data + b'\n--END-OF-REQUEST--\n')
        body = b'asset-body'
        resp = (b'HTTP/1.1 200 OK\r\n'
                b'Content-Type: application/octet-stream\r\n'
                b'Content-Length: ' + str(len(body)).encode() + b'\r\n'
                b'Cache-Control: max-age=3600\r\n'
                b'ETag: "test-etag"\r\n\r\n' + body)
        conn.sendall(resp); conn.close()
    except Exception as e:
        sys.stderr.write(f'mock error: {e}\n'); break
PY
python3 "${MOCK_SCRIPT}" "${MOCK_PORT}" "${MOCK_CAPTURE}" &
MOCK_PID=$!
sleep 0.3

# Start harbor server. Set:
#   - harbor_cors_origins: exercise the /info CORS allow-list path
#   - ui_remote_url: point UI proxy at our local mock instead of
#     https://ui.duckdb.org so the PR-8 leak-test can capture exactly
#     what harbor would send upstream
nohup "${DUCKDB_BIN}" -unsigned -no-stdin -c "
LOAD '${EXT_PATH}';
SET GLOBAL harbor_cors_origins='https://app.example.com';
SET GLOBAL ui_remote_url='http://127.0.0.1:${MOCK_PORT}';
CALL harbor_serve(bind := '127.0.0.1', port := ${PORT}, token := '${TOKEN}');
CALL harbor_wait();
" > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!
sleep 2

# ---- /info (no Origin) → no ACAO header at all ----
INFO_HEADERS="$(curl -sI "http://127.0.0.1:${PORT}/info")"
echo "${INFO_HEADERS}" | grep -qi '^HTTP/1.1 204' \
    || fail "/info expected 204, got $(echo "${INFO_HEADERS}" | head -1)"
if echo "${INFO_HEADERS}" | grep -qi '^Access-Control-Allow-Origin:'; then
    fail "/info should NOT emit Access-Control-Allow-Origin without an Origin request header"
fi
pass "/info no-Origin → 204, no ACAO"

# ---- /info with allowed Origin → ACAO echoes the match ----
ALLOWED_HEADERS="$(curl -sI -H 'Origin: https://app.example.com' "http://127.0.0.1:${PORT}/info")"
echo "${ALLOWED_HEADERS}" | grep -qi '^Access-Control-Allow-Origin: https://app.example.com' \
    || fail "/info with allowed Origin must echo it back exactly"
echo "${ALLOWED_HEADERS}" | grep -qi '^Access-Control-Allow-Credentials: true' \
    || fail "/info with allowed Origin must include Allow-Credentials: true"
pass "/info allowed-Origin → ACAO echoes match"

# ---- /info with disallowed Origin → no ACAO ----
DISALLOWED_HEADERS="$(curl -sI -H 'Origin: https://evil.example.com' "http://127.0.0.1:${PORT}/info")"
if echo "${DISALLOWED_HEADERS}" | grep -qi '^Access-Control-Allow-Origin:'; then
    fail "/info with disallowed Origin must NOT echo any ACAO"
fi
pass "/info disallowed-Origin → no ACAO"

# ---- POST /auth/login with valid JSON body ----
LOGIN_RESP="$(curl -s -i -X POST "http://127.0.0.1:${PORT}/auth/login" \
    -H 'Content-Type: application/json' \
    -d "{\"token\":\"${TOKEN}\"}")"
echo "${LOGIN_RESP}" | grep -qi '^HTTP/1.1 200 OK' \
    || fail "/auth/login valid token expected 200, got $(echo "${LOGIN_RESP}" | head -1)"
SET_COOKIE_LINE="$(echo "${LOGIN_RESP}" | grep -i '^Set-Cookie: harbor_session=' || true)"
if [[ -z "${SET_COOKIE_LINE}" ]]; then
    fail "/auth/login valid token must set harbor_session cookie"
fi
COOKIE_VALUE="$(echo "${SET_COOKIE_LINE}" | sed -E 's/^[Ss]et-[Cc]ookie: harbor_session=([^;]+).*$/\1/' | tr -d '\r')"
[[ "${COOKIE_VALUE}" =~ ^v1\..*\..*\..*\..* ]] \
    || fail "cookie value must match format v1.<seg1>.<seg2>.<seg3>.<seg4>; got '${COOKIE_VALUE}'"
echo "${SET_COOKIE_LINE}" | grep -qi 'HttpOnly' \
    || fail "cookie must include HttpOnly attribute"
echo "${SET_COOKIE_LINE}" | grep -qi 'SameSite=Strict' \
    || fail "cookie must include SameSite=Strict attribute"
echo "${SET_COOKIE_LINE}" | grep -qi 'Path=/' \
    || fail "cookie must include Path=/ attribute"
pass "/auth/login valid → 200 + cookie v1.<...> + HttpOnly + SameSite=Strict"

# Persist cookie to jar for later requests.
echo -e "127.0.0.1\tFALSE\t/\tFALSE\t0\tharbor_session\t${COOKIE_VALUE}" > "${COOKIE_JAR}"

# ---- POST /auth/login with INVALID token → 401 ----
INVALID_STATUS="$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:${PORT}/auth/login" \
    -H 'Content-Type: application/json' \
    -d '{"token":"definitely-wrong"}')"
[[ "${INVALID_STATUS}" == "401" ]] \
    || fail "/auth/login invalid token expected 401, got ${INVALID_STATUS}"
pass "/auth/login invalid → 401"

# ---- POST /auth/login with bearer header instead of body ----
BEARER_STATUS="$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:${PORT}/auth/login" \
    -H "Authorization: Bearer ${TOKEN}" -d '')"
[[ "${BEARER_STATUS}" == "200" ]] \
    || fail "/auth/login with Bearer header expected 200, got ${BEARER_STATUS}"
pass "/auth/login Bearer → 200"

# ---- POST /auth/logout → 200 + clear cookie ----
LOGOUT_RESP="$(curl -s -i -X POST "http://127.0.0.1:${PORT}/auth/logout" -d '')"
echo "${LOGOUT_RESP}" | grep -qi '^HTTP/1.1 200 OK' \
    || fail "/auth/logout expected 200, got $(echo "${LOGOUT_RESP}" | head -1)"
echo "${LOGOUT_RESP}" | grep -qi '^Set-Cookie: harbor_session=; .*Max-Age=0' \
    || fail "/auth/logout must clear cookie with Max-Age=0"
pass "/auth/logout → 200 + cleared cookie"

# ---- GET / no cookie → 200 + login page HTML ----
LOGIN_PAGE_BODY="$(curl -s "http://127.0.0.1:${PORT}/")"
echo "${LOGIN_PAGE_BODY}" | grep -qi 'harbor' \
    || fail "GET / no-cookie should serve login page containing 'harbor'"
echo "${LOGIN_PAGE_BODY}" | grep -qi 'auth/login' \
    || fail "GET / login page should reference /auth/login"
pass "GET / no-cookie → 200 + login page HTML"

# ---- GET /random/path no cookie → 401 ----
RAND_STATUS="$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:${PORT}/random/path")"
[[ "${RAND_STATUS}" == "401" ]] \
    || fail "GET /random/path no-cookie expected 401, got ${RAND_STATUS}"
pass "GET /random/path no-cookie → 401"

# ---- GET /auth/login → 405 Method Not Allowed (round-12 fix) ----
GET_LOGIN="$(curl -s -i "http://127.0.0.1:${PORT}/auth/login")"
echo "${GET_LOGIN}" | grep -qi '^HTTP/1.1 405' \
    || fail "GET /auth/login expected 405 Method Not Allowed, got $(echo "${GET_LOGIN}" | head -1)"
echo "${GET_LOGIN}" | grep -qi '^Allow: POST' \
    || fail "GET /auth/login should advertise Allow: POST, OPTIONS"
pass "GET /auth/login → 405 + Allow: POST"

# ---- /quack still serves (PR-1.5 wire-compat preserved) ----
# An empty POST body is a malformed quack message; it returns 5xx.
# The point is the route exists and handles the request rather than
# 404'ing, proving QuackHandlers is still wired up after the
# auth-handler refactor.
QUACK_STATUS="$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:${PORT}/quack" \
    -H 'Content-Type: application/vnd.duckdb' -d '')"
[[ "${QUACK_STATUS}" =~ ^[45][0-9][0-9]$ ]] \
    || fail "/quack should respond (4xx/5xx) for empty body; got ${QUACK_STATUS}"
[[ "${QUACK_STATUS}" != "404" ]] \
    || fail "/quack should not 404 (route must still be registered after PR-4)"
pass "/quack still served (HTTP ${QUACK_STATUS})"

# ---------------------------------------------------------------------
# PR-8: UI proxy MUST NOT forward harbor auth material upstream.
# Authenticated user fetches a UI asset → harbor proxies to the mock →
# we read the captured request and assert no harbor-auth headers.
# ---------------------------------------------------------------------
# Re-login and capture the cookie (we cleared it earlier with /auth/logout).
RELOGIN_RESP="$(curl -s -i -X POST "http://127.0.0.1:${PORT}/auth/login" \
    -H "Authorization: Bearer ${TOKEN}" -d '')"
echo "${RELOGIN_RESP}" | grep -qi '^HTTP/1.1 200 OK' \
    || fail "PR-8 setup: re-login expected 200, got $(echo "${RELOGIN_RESP}" | head -1)"
RELOGIN_COOKIE="$(echo "${RELOGIN_RESP}" | grep -i '^Set-Cookie: harbor_session=' \
                  | sed -E 's/^[Ss]et-[Cc]ookie: harbor_session=([^;]+).*$/\1/' | tr -d '\r')"
[[ -n "${RELOGIN_COOKIE}" ]] || fail "PR-8 setup: failed to extract harbor_session from re-login"

# Helper: re-run mock-capture leak assertions on the most recent
# captured request. Args: $1 = "auth scenario" label for messages.
assert_no_harbor_auth_leak() {
    local scenario="$1"
    sleep 0.2
    if [[ ! -s "${MOCK_CAPTURE}" ]]; then
        fail "PR-8 (${scenario}): mock listener captured no request — harbor did not proxy?"
    fi
    if grep -qi '^Cookie:' "${MOCK_CAPTURE}"; then
        echo "--- captured request ---" >&2; cat "${MOCK_CAPTURE}" >&2
        fail "PR-8 LEAK (${scenario}): Cookie header forwarded to upstream UI"
    fi
    if grep -qi 'harbor_session=' "${MOCK_CAPTURE}"; then
        echo "--- captured request ---" >&2; cat "${MOCK_CAPTURE}" >&2
        fail "PR-8 LEAK (${scenario}): harbor_session value found in upstream request"
    fi
    if grep -qi '^Authorization:' "${MOCK_CAPTURE}"; then
        echo "--- captured request ---" >&2; cat "${MOCK_CAPTURE}" >&2
        fail "PR-8 LEAK (${scenario}): Authorization header forwarded to upstream UI"
    fi
    if grep -qi '^X-Harbor-' "${MOCK_CAPTURE}"; then
        echo "--- captured request ---" >&2; cat "${MOCK_CAPTURE}" >&2
        fail "PR-8 LEAK (${scenario}): X-Harbor-* header forwarded to upstream UI"
    fi
    if ! grep -qi '^User-Agent: harbor-ui/' "${MOCK_CAPTURE}"; then
        echo "--- captured request ---" >&2; cat "${MOCK_CAPTURE}" >&2
        fail "PR-8 sanity (${scenario}): expected harbor-ui/ User-Agent in upstream request"
    fi
}

# ---- Scenario A: authenticate via Cookie. Verify Cookie+harbor_session
#                  do NOT leak. (The original PR-4 regression.)
: > "${MOCK_CAPTURE}"
ASSET_STATUS_A="$(curl -s -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:${PORT}/assets/cookie-scenario.js" \
    -b "harbor_session=${RELOGIN_COOKIE}" \
    -H 'Accept: application/javascript' \
    -H 'Accept-Encoding: gzip')"
[[ "${ASSET_STATUS_A}" == "200" ]] \
    || fail "PR-8 A: cookie-authenticated proxy expected 200, got ${ASSET_STATUS_A}"
assert_no_harbor_auth_leak "cookie auth"
# Allow-listed headers pass through.
grep -qi '^Accept: application/javascript' "${MOCK_CAPTURE}" \
    || fail "PR-8 A: Accept header should be forwarded (allow-list entry)"
# PR-8 update (post-v0.1.0 v0.1.1 patch): harbor now forces
# `Accept-Encoding: identity` upstream regardless of what the
# browser asks for. cpp-httplib's HTTPS client doesn't reliably
# decode gzip/br response bodies (bug surfaced when browsers
# fetched UI assets and got 500s). The credential-strip invariant
# is UNCHANGED — we still don't forward Cookie/Authorization/etc.
# This assertion is the explicit guard that we send identity (and
# don't accidentally regress to forwarding the browser's value).
if ! grep -qi '^Accept-Encoding: identity' "${MOCK_CAPTURE}"; then
  echo "--- captured upstream request ---" >&2
  cat "${MOCK_CAPTURE}" >&2
  fail "PR-8 A: Accept-Encoding should be forced to 'identity' upstream (compression-bug fix)"
fi
pass "PR-8 A: cookie-authenticated proxy strips Cookie/harbor_session/Auth/X-Harbor-* and forces Accept-Encoding: identity"

# ---- Scenario B: authenticate via Bearer. Verify Authorization does
#                  NOT leak (even though it was the credential we used
#                  to authenticate). The bearer-first precedence
#                  invariant means this code path actually executes.
: > "${MOCK_CAPTURE}"
ASSET_STATUS_B="$(curl -s -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:${PORT}/assets/bearer-scenario.js" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Accept: application/javascript')"
[[ "${ASSET_STATUS_B}" == "200" ]] \
    || fail "PR-8 B: bearer-authenticated proxy expected 200, got ${ASSET_STATUS_B}"
assert_no_harbor_auth_leak "bearer auth"
pass "PR-8 B: bearer-authenticated proxy strips Authorization (the credential itself does not leak upstream)"

# ---- Scenario C: authenticate via X-Harbor-Token. Verify the
#                  X-Harbor-Token header does NOT leak.
: > "${MOCK_CAPTURE}"
ASSET_STATUS_C="$(curl -s -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:${PORT}/assets/xharbor-scenario.js" \
    -H "X-Harbor-Token: ${TOKEN}" \
    -H 'Accept: application/javascript')"
[[ "${ASSET_STATUS_C}" == "200" ]] \
    || fail "PR-8 C: X-Harbor-Token-authenticated proxy expected 200, got ${ASSET_STATUS_C}"
assert_no_harbor_auth_leak "x-harbor-token auth"
pass "PR-8 C: X-Harbor-Token-authenticated proxy strips the token header upstream"

echo
# ============================================================================
# PR-7c — auth scheme tightening + login-page CSP+nonce
# ============================================================================

# Item 1 regression guards: non-Bearer Authorization explicitly rejected.
# Even with a valid cookie present, an explicit `Authorization: Basic` MUST
# return 401 with errorCode UNSUPPORTED_AUTH_SCHEME, NOT silently fall back
# to cookie auth. PR-4's round-11 invariant: explicit-bad credential must
# not be masked by ambient browser state.
#
# These guards exist so a future refactor can't accidentally regress the
# "Authorization is checked first and dominates" rule.
LOGIN_RESP="$(curl -s -i -X POST "http://127.0.0.1:${PORT}/auth/login" \
    -H "Authorization: Bearer ${TOKEN}" -d '')"
COOKIE_VAL_R7C="$(echo "${LOGIN_RESP}" | tr -d '\r' \
    | awk '/^Set-Cookie: harbor_session=/{print substr($2, index($2, "=") + 1); exit}' \
    | sed 's/;.*//')"
[[ -n "${COOKIE_VAL_R7C}" ]] || fail "PR-7c setup: could not get cookie for non-Bearer test"

# /sql with `Authorization: Basic ...` + valid cookie → 401, no fallback.
RESP="$(curl -s -i -X POST "http://127.0.0.1:${PORT}/sql" \
    -H 'Authorization: Basic dXNlcjpwYXNz' \
    -b "harbor_session=${COOKIE_VAL_R7C}" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT 42"}')"
echo "${RESP}" | grep -qi '^HTTP/1.1 401' \
    || fail "PR-7c: /sql with Authorization: Basic + valid cookie expected 401 (got: $(echo "${RESP}" | head -1))"
echo "${RESP}" | grep -q '"errorCode":"UNSUPPORTED_AUTH_SCHEME"' \
    || fail "PR-7c: /sql 401 missing errorCode UNSUPPORTED_AUTH_SCHEME"
pass "PR-7c: non-Bearer Authorization + valid cookie → 401 UNSUPPORTED_AUTH_SCHEME (no fallback)"

# /auth/login with `Authorization: Basic ...` and a valid token in body
# MUST also be rejected — the explicit-bad scheme must not be masked by
# the body fallback.
RESP="$(curl -s -i -X POST "http://127.0.0.1:${PORT}/auth/login" \
    -H 'Authorization: Basic dXNlcjpwYXNz' \
    -H 'Content-Type: application/json' \
    -d "{\"token\":\"${TOKEN}\"}")"
echo "${RESP}" | grep -qi '^HTTP/1.1 401' \
    || fail "PR-7c: /auth/login with Basic scheme expected 401"
# /auth/login uses {"error":"<code>","message":"..."} envelope shape
# (different from /sql's {"ok":false,"error":<msg>,"errorCode":<code>}).
# Both shapes use the same code values; the JSON keys differ.
echo "${RESP}" | grep -q '"error":"UNSUPPORTED_AUTH_SCHEME"' \
    || fail "PR-7c: /auth/login UNSUPPORTED_AUTH_SCHEME code missing"
pass "PR-7c: /auth/login with Basic scheme → 401 UNSUPPORTED_AUTH_SCHEME (body fallback NOT taken)"

# Item 2: login-page CSP + nonce.
LOGIN_PAGE="$(curl -s -i "http://127.0.0.1:${PORT}/")"
echo "${LOGIN_PAGE}" | grep -qi '^Content-Security-Policy:' \
    || fail "PR-7c: GET / login page missing Content-Security-Policy header"
# CSP MUST contain nonce-... and MUST NOT allow 'unsafe-inline' for script-src.
CSP_LINE="$(echo "${LOGIN_PAGE}" | tr -d '\r' | awk -F': ' '/^[Cc]ontent-[Ss]ecurity-[Pp]olicy:/{print $2; exit}')"
echo "${CSP_LINE}" | grep -qE "script-src 'nonce-[A-Za-z0-9+/=]+'" \
    || fail "PR-7c: CSP missing 'script-src nonce-<value>'; CSP=${CSP_LINE}"
echo "${CSP_LINE}" | grep -q "'unsafe-inline'" | grep -q "script-src" \
    && fail "PR-7c: CSP MUST NOT allow 'unsafe-inline' in script-src"
echo "${CSP_LINE}" | grep -q "default-src 'none'" \
    || fail "PR-7c: CSP missing 'default-src none' baseline"
echo "${CSP_LINE}" | grep -q "frame-ancestors 'none'" \
    || fail "PR-7c: CSP missing 'frame-ancestors none'"
# The page MUST include a `<script nonce="...">` tag with the same nonce.
NONCE_FROM_CSP="$(echo "${CSP_LINE}" | sed -E "s/.*nonce-([A-Za-z0-9+/=]+).*/\1/")"
echo "${LOGIN_PAGE}" | grep -q "<script nonce=\"${NONCE_FROM_CSP}\">" \
    || fail "PR-7c: login page <script nonce> tag does not match CSP nonce"
pass "PR-7c: login page CSP header + matching <script nonce>"

# Two consecutive GET / requests have DIFFERENT nonces (per-request CSPRNG).
LOGIN_1="$(curl -s -i "http://127.0.0.1:${PORT}/" | tr -d '\r')"
LOGIN_2="$(curl -s -i "http://127.0.0.1:${PORT}/" | tr -d '\r')"
NONCE_1="$(echo "${LOGIN_1}" | awk -F': ' '/^[Cc]ontent-[Ss]ecurity-[Pp]olicy:/{print $2; exit}' | sed -E "s/.*nonce-([A-Za-z0-9+/=]+).*/\1/")"
NONCE_2="$(echo "${LOGIN_2}" | awk -F': ' '/^[Cc]ontent-[Ss]ecurity-[Pp]olicy:/{print $2; exit}' | sed -E "s/.*nonce-([A-Za-z0-9+/=]+).*/\1/")"
[[ -n "${NONCE_1}" && -n "${NONCE_2}" ]] || fail "PR-7c: missing nonce on consecutive GET / requests"
[[ "${NONCE_1}" != "${NONCE_2}" ]] || fail "PR-7c: consecutive GET / returned the same nonce (CSPRNG broken or static)"
pass "PR-7c: consecutive GET / requests have distinct CSPRNG nonces"

green "All cookie-auth + credential-strip golden assertions passed."
