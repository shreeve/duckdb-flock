#!/usr/bin/env bash
# scripts/validate-deployment.sh — exercise an already-running harbor
# server end-to-end, without starting one yourself. Use after deploying
# harbor (locally, in a container, on a server) to confirm every
# protocol surface is healthy and the auth + CORS + timeout invariants
# hold.
#
# Usage:
#   scripts/validate-deployment.sh <base-url> <bearer-token> [--origin <url>]
#
# Examples:
#   scripts/validate-deployment.sh http://127.0.0.1:9494 a1b2c3d4...
#   scripts/validate-deployment.sh https://harbor.example.com $TOKEN \
#       --origin https://app.example.com
#
# Notes:
#   - The server MUST already be running. Start it via:
#       LOAD '/path/to/harbor.<platform>.duckdb_extension';
#       CALL harbor_serve('harbor:127.0.0.1:9494');
#       CALL harbor_wait();
#   - The --origin flag controls the CORS-allow-list test. If your
#     deployment has `harbor_cors_origins` set, pass one of the allowed
#     origins here. If unset, omit the flag and the CORS test is skipped.
#   - Pass/fail counts are reported at the end. Any FAIL → non-zero exit.
#
# Coverage (~30 assertions):
#   - /health, /info — basic liveness + headers
#   - /sql happy path — bearer auth, one-shot JSON response
#   - /sql NDJSON streaming — schema/row/end framing
#   - /sql with bad bearer → 401 INVALID_TOKEN
#   - /sql with non-Bearer Authorization → 401 UNSUPPORTED_AUTH_SCHEME
#   - /sql with no auth → 401 UNAUTHORIZED
#   - /sql multi-statement rejection → 400 BAD_REQUEST
#   - /sql Mode A and Mode B params (incl. nested LIST/STRUCT/MAP)
#   - GET / (login page) — CSP header + nonce
#   - /auth/login → cookie → reuse for /sql; /auth/logout
#   - OPTIONS /sql, /quack — CORS preflight (if --origin given)
#   - /quack still served (no auth here, just a smoke probe)
#   - admin /tables (default-deny vs success depending on server config)

set -euo pipefail

if [ $# -lt 2 ]; then
  echo "Usage: $0 <base-url> <bearer-token> [--origin <allowed-origin>]" >&2
  exit 64
fi

BASE_URL="${1%/}"   # strip trailing slash
TOKEN="$2"
shift 2

ORIGIN=""
while [ $# -gt 0 ]; do
  case "$1" in
    --origin) ORIGIN="$2"; shift 2 ;;
    *) echo "Unknown flag: $1" >&2; exit 64 ;;
  esac
done

PASS=0
FAIL=0
COOKIE_JAR="$(mktemp)"
trap 'rm -f "$COOKIE_JAR"' EXIT

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yellow(){ printf '\033[33m%s\033[0m\n' "$*"; }
fail() { red "FAIL: $*"; FAIL=$((FAIL + 1)); }
pass() { green "PASS: $*"; PASS=$((PASS + 1)); }
skip() { yellow "SKIP: $*"; }

# Strict-mode-friendly conditional fail (don't abort the whole script on
# the first failure; we want to enumerate everything that's wrong).
set +e

# ---- liveness + identity ----------------------------------------------------

http_status() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

CODE="$(http_status "$BASE_URL/health")"
[[ "$CODE" == "200" ]] && pass "/health → 200" || fail "/health → $CODE (expected 200)"

CODE="$(http_status "$BASE_URL/info")"
[[ "$CODE" == "204" ]] && pass "/info → 204" || fail "/info → $CODE"

# /info should expose the harbor identity headers (PR-3+).
INFO_HEADERS="$(curl -s -i "$BASE_URL/info" | tr -d '\r')"
echo "$INFO_HEADERS" | grep -qi '^X-DuckDB-Version:' \
  && pass "/info has X-DuckDB-Version" \
  || fail "/info missing X-DuckDB-Version header"
echo "$INFO_HEADERS" | grep -qi '^X-DuckDB-Platform:' \
  && pass "/info has X-DuckDB-Platform" \
  || fail "/info missing X-DuckDB-Platform header"

# ---- /sql happy path --------------------------------------------------------

RESP="$(curl -s -X POST "$BASE_URL/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d '{"sql":"SELECT 42 AS the_answer"}')"
echo "$RESP" | grep -q '"the_answer"' \
  && echo "$RESP" | grep -q '"data":\[\[42\]\]' \
  && pass "/sql one-shot JSON: SELECT 42" \
  || fail "/sql one-shot: ${RESP}"

# Streaming NDJSON — should emit schema, row, end on separate lines.
NDJSON="$(curl -s -N -X POST "$BASE_URL/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT i FROM range(3) AS t(i)"}')"
SCHEMA_LINES="$(echo "$NDJSON" | grep -c '^{"type":"schema"' || true)"
ROW_LINES="$(echo "$NDJSON" | grep -c '^{"type":"row"' || true)"
END_LINES="$(echo "$NDJSON" | grep -c '^{"type":"end"' || true)"
[[ "$SCHEMA_LINES" -eq 1 && "$ROW_LINES" -eq 3 && "$END_LINES" -eq 1 ]] \
  && pass "/sql NDJSON streaming: 1 schema, 3 rows, 1 end" \
  || fail "/sql NDJSON unexpected framing (schema=$SCHEMA_LINES rows=$ROW_LINES end=$END_LINES)"

# ---- auth invariants --------------------------------------------------------

CODE="$(http_status -X POST "$BASE_URL/sql" \
  -H "Authorization: Bearer NOT_A_REAL_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT 1"}')"
[[ "$CODE" == "401" ]] && pass "bad bearer → 401" || fail "bad bearer → $CODE (expected 401)"

CODE="$(http_status -X POST "$BASE_URL/sql" \
  -H "Authorization: Basic dXNlcjpwYXNz" \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT 1"}')"
[[ "$CODE" == "401" ]] && pass "non-Bearer Authorization → 401" \
  || fail "non-Bearer Authorization → $CODE (expected 401 UNSUPPORTED_AUTH_SCHEME)"

CODE="$(http_status -X POST "$BASE_URL/sql" \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT 1"}')"
[[ "$CODE" == "401" ]] && pass "no auth → 401" || fail "no auth → $CODE"

# ---- request validation -----------------------------------------------------

CODE="$(http_status -X POST "$BASE_URL/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT 1; SELECT 2"}')"
[[ "$CODE" == "400" ]] && pass "multi-statement /sql → 400" \
  || fail "multi-statement /sql → $CODE (expected 400)"

CODE="$(http_status -X POST "$BASE_URL/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"params":[42]}')"
[[ "$CODE" == "400" ]] && pass "missing sql field → 400" \
  || fail "missing sql field → $CODE"

# ---- params: Mode A scalar, Mode B nested ----------------------------------

RESP="$(curl -s -X POST "$BASE_URL/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d '{"sql":"SELECT $1::INTEGER + $2::INTEGER AS sum","params":[2,3]}')"
echo "$RESP" | grep -q '"data":\[\[5\]\]' \
  && pass "Mode A params (implicit int)" \
  || fail "Mode A params: $RESP"

RESP="$(curl -s -X POST "$BASE_URL/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d '{"sql":"SELECT $1 AS x","params":[{"type":"DECIMAL(18,4)","value":"123.4567"}]}')"
echo "$RESP" | grep -q '"123.4567"' \
  && pass "Mode B params (DECIMAL)" \
  || fail "Mode B DECIMAL: $RESP"

RESP="$(curl -s -X POST "$BASE_URL/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d '{"sql":"SELECT $1 AS x","params":[{"type":"LIST<INTEGER>","value":[1,2,3]}]}')"
echo "$RESP" | grep -q '\[1,2,3\]' \
  && pass "Mode B params (LIST<INTEGER>)" \
  || fail "Mode B LIST: $RESP"

RESP="$(curl -s -X POST "$BASE_URL/sql" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d '{"sql":"SELECT $1 AS x","params":[{"type":"STRUCT(a INTEGER, b VARCHAR)","value":{"a":1,"b":"hi"}}]}')"
echo "$RESP" | grep -q '"a":1' && echo "$RESP" | grep -q '"b":"hi"' \
  && pass "Mode B params (STRUCT)" \
  || fail "Mode B STRUCT: $RESP"

# ---- login page CSP + nonce -----------------------------------------------

LOGIN_PAGE="$(curl -s -i "$BASE_URL/" | tr -d '\r')"
echo "$LOGIN_PAGE" | grep -qi '^Content-Security-Policy:' \
  && pass "GET / Content-Security-Policy header present" \
  || fail "GET / missing Content-Security-Policy"

CSP="$(echo "$LOGIN_PAGE" | awk -F': ' '/^[Cc]ontent-[Ss]ecurity-[Pp]olicy:/{print $2; exit}')"
echo "$CSP" | grep -qE "script-src 'nonce-[A-Za-z0-9+/=]+'" \
  && pass "CSP contains script-src nonce-<value>" \
  || fail "CSP missing nonce-<value> (got: $CSP)"

echo "$CSP" | grep -q "default-src 'none'" \
  && pass "CSP has default-src 'none' baseline" \
  || fail "CSP missing default-src 'none'"

NONCE_FROM_CSP="$(echo "$CSP" | sed -E "s/.*nonce-([A-Za-z0-9+/=]+).*/\1/")"
echo "$LOGIN_PAGE" | grep -q "<script nonce=\"$NONCE_FROM_CSP\">" \
  && pass "<script nonce> matches CSP nonce" \
  || fail "<script nonce> does not match CSP"

# ---- cookie roundtrip: login → cookie → /sql with cookie → logout -----------

LOGIN_RESP="$(curl -s -i -X POST "$BASE_URL/auth/login" \
  -H "Authorization: Bearer $TOKEN" -d '')"
COOKIE_VAL="$(echo "$LOGIN_RESP" | tr -d '\r' \
  | awk '/^Set-Cookie: harbor_session=/{print substr($2, index($2, "=") + 1); exit}' \
  | sed 's/;.*//')"
[[ -n "$COOKIE_VAL" ]] && pass "/auth/login → harbor_session cookie issued" \
  || fail "/auth/login did not set harbor_session"

if [[ -n "$COOKIE_VAL" ]]; then
  # Cookie-authenticated /sql requires an Origin header (CSRF defense
  # per SPEC §7 — without this any random page in a browser could
  # POST to /sql with the ambient cookie attached). Same-origin
  # ($BASE_URL itself) is always allowed; if the deployment has
  # `harbor_cors_origins` set, any entry in that allow-list also works.
  CODE="$(http_status -X POST "$BASE_URL/sql" \
    -b "harbor_session=$COOKIE_VAL" \
    -H "Origin: $BASE_URL" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT 7"}')"
  [[ "$CODE" == "200" ]] && pass "/sql with cookie + same-origin Origin → 200" \
    || fail "/sql with cookie + same-origin Origin → $CODE (expected 200)"

  # And a CSRF-defense regression guard — cookie WITHOUT Origin must
  # be rejected with 403 FORBIDDEN. This is the load-bearing invariant
  # that makes ambient cookies safe.
  CODE="$(http_status -X POST "$BASE_URL/sql" \
    -b "harbor_session=$COOKIE_VAL" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT 7"}')"
  [[ "$CODE" == "403" ]] && pass "/sql with cookie + NO Origin → 403 (CSRF defense holds)" \
    || fail "/sql with cookie + NO Origin → $CODE (expected 403 FORBIDDEN)"

  LOGOUT_RESP="$(curl -s -i -X POST "$BASE_URL/auth/logout" \
    -b "harbor_session=$COOKIE_VAL" -d '')"
  echo "$LOGOUT_RESP" | grep -q 'Set-Cookie: harbor_session=.*Max-Age=0' \
    && pass "/auth/logout clears cookie" \
    || fail "/auth/logout did not clear cookie"
fi

# ---- CORS allow-list (if --origin given) ------------------------------------

if [[ -n "$ORIGIN" ]]; then
  PREFLIGHT="$(curl -s -i -X OPTIONS "$BASE_URL/sql" \
    -H "Origin: $ORIGIN" \
    -H 'Access-Control-Request-Method: POST' \
    -H 'Access-Control-Request-Headers: authorization, content-type')"
  echo "$PREFLIGHT" | grep -qi "^Access-Control-Allow-Origin: $ORIGIN" \
    && pass "OPTIONS /sql with allowed origin → ACAO echoes back" \
    || fail "OPTIONS /sql with --origin $ORIGIN: ACAO header missing or mismatched"

  PREFLIGHT="$(curl -s -i -X OPTIONS "$BASE_URL/sql" \
    -H 'Origin: https://attacker.example.com' \
    -H 'Access-Control-Request-Method: POST')"
  echo "$PREFLIGHT" | grep -qi '^Access-Control-Allow-Origin:' \
    && fail "disallowed origin: ACAO header should NOT be set" \
    || pass "OPTIONS /sql with disallowed origin → no ACAO header"
else
  skip "CORS allow-list test (pass --origin <url> to enable)"
fi

# ---- /quack smoke -----------------------------------------------------------

# /quack expects binary protocol; a plain GET should yield a method-not-allowed
# or 4xx, not a 5xx. We just want to confirm the route is registered.
CODE="$(http_status -X OPTIONS "$BASE_URL/quack")"
[[ "$CODE" == "204" || "$CODE" == "200" ]] \
  && pass "OPTIONS /quack → $CODE (route registered)" \
  || fail "OPTIONS /quack → $CODE"

# ---- admin endpoint behavior -----------------------------------------------

CODE="$(http_status "$BASE_URL/tables" -H "Authorization: Bearer $TOKEN")"
case "$CODE" in
  200) pass "/tables → 200 (harbor_allow_admin_without_authz=true OR custom authz allows)" ;;
  403) pass "/tables → 403 (default-deny is active — admin endpoints gated)" ;;
  *)   fail "/tables → $CODE (expected 200 or 403)" ;;
esac

CODE="$(http_status "$BASE_URL/whoami" -H "Authorization: Bearer $TOKEN")"
case "$CODE" in
  200) pass "/whoami → 200" ;;
  403) pass "/whoami → 403 (default-deny)" ;;
  *)   fail "/whoami → $CODE" ;;
esac

# ---- summary ---------------------------------------------------------------

set -e
echo
TOTAL=$((PASS + FAIL))
if [[ $FAIL -eq 0 ]]; then
  green "Validation OK: $PASS / $TOTAL assertions passed."
  exit 0
else
  red "Validation FAILED: $FAIL of $TOTAL assertions failed ($PASS passed)."
  exit 1
fi
