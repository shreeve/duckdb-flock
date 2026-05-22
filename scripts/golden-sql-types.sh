#!/usr/bin/env bash
# PR-7e — comprehensive per-DuckDB-type /sql encoding round-trip golden test.
#
# This is the v0.1 type-coverage net: for every documented DuckDB type,
# send `SELECT <literal>::<TYPE>` over /sql and assert the schema line
# (duckdbType, lossless) AND the row value match SPEC §5.4 exactly.
#
# Designed in GPT-5.5 R-27. Critical additions over the original plan:
#   - All unsigned integer families (UTINYINT…UHUGEINT)
#   - HUGEINT
#   - DOUBLE non-finite: NaN, Infinity, -Infinity
#   - NULL for nested containers (list element, struct field, map value)
#   - Empty containers ([]::INTEGER[], empty map)
#   - JSON SQL NULL vs JSON 'null' literal (distinct semantics)
#   - BLOB zero bytes, non-UTF-8 bytes
#   - VARCHAR with quotes, backslash, newline, multi-byte Unicode, emoji
#   - ENUM (user-defined type)
#
# Each test asserts BOTH schema-type AND value encoding. Asserting only
# the value risks a regression where the encoder lies about the schema
# while still emitting a plausible value.
#
# Usage:
#   make release
#   scripts/golden-sql-types.sh
#
# Exit code: 0 on success; non-zero on any assertion failure.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT_PATH="${REPO_ROOT}/build/release/extension/harbor/harbor.duckdb_extension"
DUCKDB_BIN="${REPO_ROOT}/build/release/duckdb"
PORT="${HARBOR_TEST_PORT:-19501}"
TOKEN="sql-types-golden-token-$$"
SERVER_LOG="$(mktemp)"
SERVER_PID=""
PASS_COUNT=0

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    # SIGTERM (default) is what duckdb-harbor's harbor_wait() blocks on.
    # SIGINT can be intercepted and not propagate cleanly through the
    # background process group — use TERM to avoid the cleanup hanging.
    kill "${SERVER_PID}" 2>/dev/null || true
    # Don't wait synchronously — if the server is unresponsive we
    # still want to exit. The OS will reap.
  fi
  rm -f "${SERVER_LOG}" 2>/dev/null || true
}
trap cleanup EXIT

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
fail() { red "FAIL: $*"; echo "--- server log tail ---"; tail -n 30 "${SERVER_LOG}" || true; exit 1; }
pass() { green "PASS: $*"; PASS_COUNT=$((PASS_COUNT + 1)); }

# Start the server. `harbor_allow_admin_without_authz=true` is required
# so the script can create explicit /sql/sessions for the ENUM-UDT test
# (which needs sessionful state to CREATE TYPE then SELECT). This is
# the same convention used by golden-sql-roundtrip.sh.
nohup "${DUCKDB_BIN}" -unsigned -no-stdin -c "
LOAD '${EXT_PATH}';
SET GLOBAL harbor_allow_admin_without_authz=true;
CALL harbor_serve('quack:127.0.0.1:${PORT}', token := '${TOKEN}');
CALL harbor_wait();
" > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

# Wait for server.
for _ in $(seq 1 30); do
  sleep 0.2
  if curl -sf "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
    break
  fi
done
curl -sf "http://127.0.0.1:${PORT}/health" >/dev/null \
  || fail "server did not come up on port ${PORT}"

# `assert_type SQL_FRAGMENT EXPECTED_DUCKDB_TYPE EXPECTED_LOSSLESS EXPECTED_VALUE_JSON LABEL`
#
# Sends `SELECT <SQL_FRAGMENT> AS x` as one-shot JSON, then asserts the
# top-level schema entry and the encoded value. EXPECTED_VALUE_JSON is
# the LITERAL JSON text the value should encode to (e.g. `42` for a
# safe BIGINT number, `"9007199254740992"` for an unsafe BIGINT string,
# `null` for SQL NULL).
assert_type() {
  local sql="$1"
  local expected_type="$2"
  local expected_lossless="$3"
  local expected_value="$4"
  local label="$5"

  local body
  body="$(jq -nc --arg sql "SELECT ${sql} AS x" '{sql: $sql}')"

  local resp
  resp="$(curl -sf -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/json' \
    -d "${body}")" || fail "${label}: HTTP request failed"

  # Extract the schema row's duckdbType + lossless.
  # The one-shot JSON envelope shape (Accept: application/json) is
  # {"ok": true, "kind": "select", "columns": [...], "data": [[...]], "rowCount": N, "timeMs": ms}.
  # The NDJSON shape (default Accept) emits a separate `schema` line.
  local got_type got_lossless got_value
  got_type="$(echo "${resp}" | jq -r '.columns[0].duckdbType // ""')"
  got_lossless="$(echo "${resp}" | jq -r '.columns[0].lossless // ""')"
  got_value="$(echo "${resp}" | jq -c '.data[0][0]')"

  [[ "${got_type}" == "${expected_type}" ]] \
    || fail "${label}: duckdbType expected '${expected_type}', got '${got_type}' (resp: ${resp})"
  [[ "${got_lossless}" == "${expected_lossless}" ]] \
    || fail "${label}: lossless expected '${expected_lossless}', got '${got_lossless}'"
  [[ "${got_value}" == "${expected_value}" ]] \
    || fail "${label}: value expected '${expected_value}', got '${got_value}' (resp: ${resp})"
  pass "${label}"
}

# ============================================================================
# Booleans
# ============================================================================
assert_type "TRUE"                    "BOOLEAN"  "true" "true"  "BOOLEAN true"
assert_type "FALSE"                   "BOOLEAN"  "true" "false" "BOOLEAN false"
assert_type "NULL::BOOLEAN"           "BOOLEAN"  "true" "null"  "BOOLEAN NULL"

# ============================================================================
# Signed integer families
# ============================================================================
assert_type "1::TINYINT"              "TINYINT"  "true" "1"     "TINYINT"
assert_type "1::SMALLINT"             "SMALLINT" "true" "1"     "SMALLINT"
assert_type "1::INTEGER"              "INTEGER"  "true" "1"     "INTEGER"
assert_type "1::BIGINT"               "BIGINT"   "true" "1"     "BIGINT small (number)"
assert_type "1::HUGEINT"              "HUGEINT"  "true" "1"     "HUGEINT small (number)"
# Boundary values.
assert_type "127::TINYINT"            "TINYINT"  "true" "127"   "TINYINT max"
assert_type "(-128)::TINYINT"         "TINYINT"  "true" "-128"  "TINYINT min"
assert_type "9007199254740991::BIGINT"       "BIGINT"  "true" "9007199254740991"      "BIGINT max safe integer (number)"
assert_type "9007199254740992::BIGINT"       "BIGINT"  "true" "\"9007199254740992\""  "BIGINT just above safe integer (string)"
assert_type "(-9007199254740991)::BIGINT"    "BIGINT"  "true" "-9007199254740991"     "BIGINT min safe integer (number)"
assert_type "(-9007199254740992)::BIGINT"    "BIGINT"  "true" "\"-9007199254740992\"" "BIGINT just below safe integer (string)"
assert_type "9223372036854775807::BIGINT"    "BIGINT"  "true" "\"9223372036854775807\"" "BIGINT max (string)"
assert_type "(-9223372036854775808)::BIGINT" "BIGINT"  "true" "\"-9223372036854775808\"" "BIGINT min (string)"
assert_type "9007199254740991::HUGEINT"       "HUGEINT" "true" "9007199254740991"       "HUGEINT max safe integer (number)"
assert_type "9007199254740992::HUGEINT"       "HUGEINT" "true" "\"9007199254740992\""   "HUGEINT just above safe integer (string)"
assert_type "(-9007199254740991)::HUGEINT"    "HUGEINT" "true" "-9007199254740991"      "HUGEINT min safe integer (number)"
assert_type "(-9007199254740992)::HUGEINT"    "HUGEINT" "true" "\"-9007199254740992\""  "HUGEINT just below safe integer (string)"
assert_type "170141183460469231731687303715884105727::HUGEINT" "HUGEINT" "true" "\"170141183460469231731687303715884105727\"" "HUGEINT max (string)"
assert_type "(-170141183460469231731687303715884105728)::HUGEINT" "HUGEINT" "true" "\"-170141183460469231731687303715884105728\"" "HUGEINT min (string)"

# ============================================================================
# Unsigned integer families (R-27 catch — every family covered)
# ============================================================================
assert_type "1::UTINYINT"             "UTINYINT"  "true"  "1"     "UTINYINT"
assert_type "1::USMALLINT"            "USMALLINT" "true"  "1"     "USMALLINT"
assert_type "1::UINTEGER"             "UINTEGER"  "true"  "1"     "UINTEGER"
assert_type "1::UBIGINT"              "UBIGINT"   "true"  "1"     "UBIGINT small (number)"
assert_type "1::UHUGEINT"             "UHUGEINT"  "true"  "1"     "UHUGEINT small (number)"
assert_type "255::UTINYINT"           "UTINYINT"  "true"  "255"   "UTINYINT max"
assert_type "9007199254740991::UBIGINT"  "UBIGINT"  "true" "9007199254740991"      "UBIGINT max safe integer (number)"
assert_type "9007199254740992::UBIGINT"  "UBIGINT"  "true" "\"9007199254740992\""  "UBIGINT just above safe integer (string)"
assert_type "18446744073709551615::UBIGINT" "UBIGINT" "true" "\"18446744073709551615\"" "UBIGINT max (string)"
assert_type "9007199254740991::UHUGEINT" "UHUGEINT" "true" "9007199254740991"      "UHUGEINT max safe integer (number)"
assert_type "9007199254740992::UHUGEINT" "UHUGEINT" "true" "\"9007199254740992\""  "UHUGEINT just above safe integer (string)"
assert_type "340282366920938463463374607431768211455::UHUGEINT" "UHUGEINT" "true" "\"340282366920938463463374607431768211455\"" "UHUGEINT max (string)"

# ============================================================================
# Floating point (incl. R-27 non-finite checks)
# ============================================================================
assert_type "1.5::FLOAT"              "FLOAT"  "true" "1.5"     "FLOAT happy"
assert_type "1.5::DOUBLE"             "DOUBLE" "true" "1.5"     "DOUBLE happy"
# IEEE 754 specials per SPEC §5.4: encoded as JSON strings since JSON
# doesn't natively represent NaN/Inf.
assert_type "'nan'::DOUBLE"           "DOUBLE" "true" "\"NaN\""       "DOUBLE NaN"
assert_type "'inf'::DOUBLE"           "DOUBLE" "true" "\"Infinity\""  "DOUBLE +Infinity"
assert_type "'-inf'::DOUBLE"          "DOUBLE" "true" "\"-Infinity\"" "DOUBLE -Infinity"

# ============================================================================
# DECIMAL — string-encoded for precision
# ============================================================================
assert_type "123.4567::DECIMAL(18,4)"        "DECIMAL(18,4)"  "true" "\"123.4567\"" "DECIMAL(18,4)"
assert_type "1234567890.123456789012::DECIMAL(38,12)" "DECIMAL(38,12)" "true" "\"1234567890.123456789012\"" "DECIMAL(38,12) wide"
assert_type "NULL::DECIMAL(18,4)"            "DECIMAL(18,4)"  "true" "null"          "DECIMAL NULL"

# ============================================================================
# VARCHAR — Unicode + escapes
# ============================================================================
assert_type "'hello'"                          "VARCHAR" "true" "\"hello\""    "VARCHAR ASCII"
assert_type "'caf' || chr(233)"                "VARCHAR" "true" "\"café\""     "VARCHAR Latin-1 unicode"
assert_type "chr(128512)"                      "VARCHAR" "true" "\"😀\""        "VARCHAR emoji"
assert_type "'line1' || chr(10) || 'line2'"    "VARCHAR" "true" "\"line1\\nline2\"" "VARCHAR with newline"
assert_type "'a' || chr(34) || 'b'"            "VARCHAR" "true" "\"a\\\"b\""   "VARCHAR with double-quote"
assert_type "'a' || chr(92) || 'b'"            "VARCHAR" "true" "\"a\\\\b\""   "VARCHAR with backslash"
assert_type "''"                               "VARCHAR" "true" "\"\""         "VARCHAR empty"

# ============================================================================
# UUID — canonical 8-4-4-4-12 hex form
# ============================================================================
assert_type "'00112233-4455-6677-8899-aabbccddeeff'::UUID" "UUID" "true" "\"00112233-4455-6677-8899-aabbccddeeff\"" "UUID lowercase"

# ============================================================================
# Date / time families
# ============================================================================
assert_type "DATE '2026-05-16'"              "DATE"      "true" "\"2026-05-16\"" "DATE"
assert_type "TIME '12:34:56'"                "TIME"      "true" "\"12:34:56\""   "TIME (no fraction)"
assert_type "TIME '12:34:56.789'"            "TIME"      "true" "\"12:34:56.789\"" "TIME with fraction"
assert_type "TIMETZ '12:34:56+02:00'"        "TIME WITH TIME ZONE" "true" "\"12:34:56+02\"" "TIMETZ (offset trims :00)"
# TIMESTAMP family uses ISO-8601 `T` separator (DuckDB canonical form).
assert_type "TIMESTAMP '2026-05-16 12:34:56'"        "TIMESTAMP"     "true" "\"2026-05-16T12:34:56\""        "TIMESTAMP"
# TIMESTAMP_{S,MS,NS} literal forms in DuckDB take epoch values, not
# calendar strings. Cast a TIMESTAMP literal to the precision-bound
# variant for human-readable canonical-form testing.
assert_type "CAST(TIMESTAMP '2026-05-16 12:34:56' AS TIMESTAMP_S)"      "TIMESTAMP_S"  "true" "\"2026-05-16T12:34:56\""        "TIMESTAMP_S"
assert_type "CAST(TIMESTAMP '2026-05-16 12:34:56.789' AS TIMESTAMP_MS)" "TIMESTAMP_MS" "true" "\"2026-05-16T12:34:56.789\""    "TIMESTAMP_MS"
assert_type "CAST(TIMESTAMP '2026-05-16 12:34:56' AS TIMESTAMP_NS)"     "TIMESTAMP_NS" "true" "\"2026-05-16T12:34:56\""        "TIMESTAMP_NS"
assert_type "TIMESTAMPTZ '2026-05-16 12:34:56+02:00'" "TIMESTAMP WITH TIME ZONE" "true" "\"2026-05-16T10:34:56Z\"" "TIMESTAMPTZ (normalized to UTC, Z suffix)"

# ============================================================================
# INTERVAL — {months, days, micros} with micros as STRING
# ============================================================================
assert_type "INTERVAL '1 day'"                "INTERVAL" "true" '{"months":0,"days":1,"micros":"0"}'   "INTERVAL day"
assert_type "INTERVAL '2 months 3 days 4 hours'" "INTERVAL" "true" '{"months":2,"days":3,"micros":"14400000000"}' "INTERVAL combined"

# ============================================================================
# BLOB — base64 encoding (incl. R-27: zero bytes, non-UTF-8 bytes)
# ============================================================================
assert_type "'hello'::BLOB"                   "BLOB" "true" "\"aGVsbG8=\""     "BLOB ascii"
assert_type "'\\x00\\x01\\x02'::BLOB"         "BLOB" "true" "\"AAEC\""         "BLOB zero+low bytes"
assert_type "'\\xff\\xfe\\xfd'::BLOB"         "BLOB" "true" "\"//79\""         "BLOB high non-UTF-8 bytes"
assert_type "''::BLOB"                        "BLOB" "true" "\"\""             "BLOB empty"

# ============================================================================
# BIT — text-encoded
# ============================================================================
assert_type "'1010'::BIT"                     "BIT" "true" "\"1010\""          "BIT four bits"

# ============================================================================
# JSON — SPEC §5.4: encoded as JSON-text STRING (not nested)
# Distinguishes SQL NULL (top-level null) from JSON 'null' literal (string).
# ============================================================================
assert_type "'{\"a\":1}'::JSON"               "JSON" "true" "\"{\\\"a\\\":1}\"" "JSON object as text-string"
assert_type "'null'::JSON"                    "JSON" "true" "\"null\""          "JSON 'null' literal as string"
assert_type "NULL::JSON"                      "JSON" "true" "null"              "SQL NULL of JSON column"

# ============================================================================
# Container types
# ============================================================================
assert_type "[1,2,3]::INTEGER[]"              "INTEGER[]"    "true" "[1,2,3]"     "LIST<INTEGER>"
assert_type "[1,NULL,3]::INTEGER[]"           "INTEGER[]"    "true" "[1,null,3]"  "LIST<INTEGER> with null element"
assert_type "[]::INTEGER[]"                   "INTEGER[]"    "true" "[]"          "LIST<INTEGER> empty"
assert_type "[1,2,3]::INTEGER[3]"             "INTEGER[3]"   "true" "[1,2,3]"     "ARRAY<INTEGER, 3>"
assert_type "{a:1, b:'x'}::STRUCT(a INTEGER, b VARCHAR)" \
    "STRUCT(a INTEGER, b VARCHAR)" "true" '{"a":1,"b":"x"}' "STRUCT happy"
assert_type "{a:1, b:NULL}::STRUCT(a INTEGER, b VARCHAR)" \
    "STRUCT(a INTEGER, b VARCHAR)" "true" '{"a":1,"b":null}' "STRUCT with NULL field"

# MAP — array-of-pairs per SPEC §5.4. R-27 catch: distinguish from JSON object.
assert_type "MAP{'a':1, 'b':2}"               "MAP(VARCHAR, INTEGER)" "true" '[["a",1],["b",2]]' "MAP<VARCHAR,INTEGER>"
assert_type "MAP{'a':1, 'b':NULL}"            "MAP(VARCHAR, INTEGER)" "true" '[["a",1],["b",null]]' "MAP with NULL value"

# ============================================================================
# ENUM (user-defined type) — R-27 catch: UDTs survive the encoder
# ============================================================================
# CREATE TYPE in a sessionful request, then SELECT a value of the type.
SID_RESP="$(curl -s -X POST "http://127.0.0.1:${PORT}/sql/sessions/new" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -d '{}')"
SID="$(echo "${SID_RESP}" | jq -r '.sessionId // empty')"
[[ -n "${SID}" ]] || fail "ENUM setup: failed to create sessionId (resp: ${SID_RESP})"
curl -sf -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -d "{\"sql\":\"CREATE TYPE mood AS ENUM ('happy','sad','ok')\",\"sessionId\":\"${SID}\"}" >/dev/null
RESP="$(curl -sf -X POST "http://127.0.0.1:${PORT}/sql" \
    -H "Authorization: Bearer ${TOKEN}" \
    -H 'Content-Type: application/json' \
    -H 'Accept: application/json' \
    -d "{\"sql\":\"SELECT 'happy'::mood AS x\",\"sessionId\":\"${SID}\"}")"
got_type="$(echo "${RESP}" | jq -r '.columns[0].duckdbType // ""')"
got_value="$(echo "${RESP}" | jq -c '.data[0][0]')"
# DuckDB renders ENUM types as ENUM('a', 'b', ...) when not aliased
# back to the user-type name. Either form is acceptable here — the
# point is just to exercise the UDT path so we catch regressions
# where ENUM coercion silently drops to varchar-cast.
[[ "${got_type}" == "mood" || "${got_type}" =~ ^ENUM ]] \
    || fail "ENUM duckdbType expected 'mood' or ENUM(...) form, got '${got_type}' (resp: ${RESP})"
[[ "${got_value}" == '"happy"' ]] \
    || fail "ENUM value expected '\"happy\"', got '${got_value}'"
pass "ENUM (UDT) survives schema + value encoding"

# Tear down session.
curl -sf -X DELETE "http://127.0.0.1:${PORT}/sql/sessions/${SID}" \
    -H "Authorization: Bearer ${TOKEN}" >/dev/null

echo
green "All ${PASS_COUNT} per-DuckDB-type /sql encoding assertions passed."
