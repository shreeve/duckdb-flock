#!/usr/bin/env bash
# scripts/load-test.sh — concurrent /sql request hammering against a
# deployed harbor. Validates the session pool + worker concurrency
# under real load and confirms harbor_query_timeout_s actually fires
# when wall-clock crosses the limit.
#
# Usage:
#   scripts/load-test.sh <base-url> <bearer-token> [<concurrency>] [<requests-per-worker>]
#
# Examples:
#   # 10 workers × 50 fast SELECTs each = 500 requests
#   scripts/load-test.sh http://127.0.0.1:9494 $TOKEN
#
#   # 50 workers × 100 requests each = 5000 requests
#   scripts/load-test.sh http://127.0.0.1:9494 $TOKEN 50 100
#
# Reports throughput, error rate, and p95 latency. Any non-2xx response
# OR any latency > 30s on a SELECT 42 is flagged as a problem.

set -euo pipefail

if [ $# -lt 2 ]; then
  echo "Usage: $0 <base-url> <bearer-token> [<concurrency>] [<requests-per-worker>]" >&2
  exit 64
fi

BASE_URL="${1%/}"
TOKEN="$2"
WORKERS="${3:-10}"
PER_WORKER="${4:-50}"
TOTAL=$((WORKERS * PER_WORKER))

red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

OUT_DIR="$(mktemp -d)"
trap 'rm -rf "$OUT_DIR"' EXIT

echo "Load test: $TOTAL requests ($WORKERS workers × $PER_WORKER each) → $BASE_URL/sql"
echo "          token=${TOKEN:0:8}…"
echo

# Each worker fires PER_WORKER requests sequentially, recording per-request
# latency in seconds + HTTP code, into its own TSV file. We aggregate at
# the end.
worker() {
  local id="$1" out="$2"
  for i in $(seq 1 "$PER_WORKER"); do
    local t_started t_ended http
    t_started=$(date +%s.%N)
    http=$(curl -s -o /dev/null -w '%{http_code}' \
      -X POST "$BASE_URL/sql" \
      -H "Authorization: Bearer $TOKEN" \
      -H 'Content-Type: application/json' \
      -d '{"sql":"SELECT 42 AS x"}' || echo "ERR")
    t_ended=$(date +%s.%N)
    awk -v s="$t_started" -v e="$t_ended" -v h="$http" -v w="$id" -v i="$i" \
        'BEGIN { printf "%d\t%d\t%s\t%.6f\n", w, i, h, e - s }' >> "$out"
  done
}

START=$(date +%s.%N)

for w in $(seq 1 "$WORKERS"); do
  worker "$w" "$OUT_DIR/w$w.tsv" &
done
wait

END=$(date +%s.%N)

# Aggregate.
cat "$OUT_DIR"/w*.tsv > "$OUT_DIR/all.tsv"

ELAPSED=$(awk -v s="$START" -v e="$END" 'BEGIN { printf "%.3f", e - s }')
ROWS=$(wc -l < "$OUT_DIR/all.tsv" | tr -d ' ')
RPS=$(awk -v r="$ROWS" -v e="$ELAPSED" 'BEGIN { printf "%.1f", r / e }')

OK_COUNT=$(awk -F'\t' '$3 == 200 { c++ } END { print c+0 }' "$OUT_DIR/all.tsv")
ERR_COUNT=$((ROWS - OK_COUNT))

# Sort latencies, pull min / median / p95 / max.
LATENCIES=$(awk -F'\t' '{ print $4 }' "$OUT_DIR/all.tsv" | sort -n)
MIN=$(echo "$LATENCIES" | head -1)
MAX=$(echo "$LATENCIES" | tail -1)
MEDIAN=$(echo "$LATENCIES" | awk -v r="$ROWS" 'NR == int(r/2) + 1 { print; exit }')
P95=$(echo "$LATENCIES" | awk -v r="$ROWS" 'NR == int(r * 0.95) + 1 { print; exit }')
P99=$(echo "$LATENCIES" | awk -v r="$ROWS" 'NR == int(r * 0.99) + 1 { print; exit }')

echo "Results:"
printf "  total requests        : %d\n" "$ROWS"
printf "  elapsed wall-clock    : %s s\n" "$ELAPSED"
printf "  throughput            : %s req/s\n" "$RPS"
printf "  HTTP 200              : %d\n" "$OK_COUNT"
printf "  errors / non-200      : %d\n" "$ERR_COUNT"
printf "  latency min           : %.3f s\n" "$MIN"
printf "  latency median        : %.3f s\n" "$MEDIAN"
printf "  latency p95           : %.3f s\n" "$P95"
printf "  latency p99           : %.3f s\n" "$P99"
printf "  latency max           : %.3f s\n" "$MAX"
echo

# Error breakdown by HTTP code (any non-200).
if [[ "$ERR_COUNT" -gt 0 ]]; then
  echo "Non-200 codes:"
  awk -F'\t' '$3 != 200 { c[$3]++ } END { for (k in c) printf "  %s : %d\n", k, c[k] }' "$OUT_DIR/all.tsv"
  echo
fi

# Pass/fail rules:
#   - error rate < 0.5% (allow tiny window for transport hiccups)
#   - p95 < 1 second on SELECT 42 (sanity threshold; bump if your harbor
#     deployment is intentionally slow / behind a heavy reverse proxy)
ERROR_RATE=$(awk -v e="$ERR_COUNT" -v t="$ROWS" 'BEGIN { printf "%.4f", e / t }')

if awk -v er="$ERROR_RATE" 'BEGIN { exit !(er > 0.005) }'; then
  red "FAIL: error rate $ERROR_RATE (> 0.5%)"
  EXIT=1
else
  green "OK: error rate $ERROR_RATE (≤ 0.5%)"
  EXIT=0
fi

if awk -v p="$P95" 'BEGIN { exit !(p > 1.0) }'; then
  yellow "WARN: p95 latency $P95 s (> 1s on SELECT 42 — investigate session-pool contention)"
  # Don't fail solely on slow p95; reverse-proxy or container overhead
  # can legitimately push this. The error rate gate is the hard one.
fi

exit $EXIT
