#!/usr/bin/env bash
# scripts/load-test.sh — concurrent /sql request load test against a
# deployed harbor.
#
# Auto-detects a real HTTP benchmarker and uses it when available:
#
#     1. oha   — Rust, modern, https://github.com/hatoo/oha
#     2. wrk   — C,    classic, https://github.com/wg/wrk
#
# When neither is installed, falls back to a pure-shell curl loop
# with a loud banner explaining that the numbers will be conservative
# (the per-request fork+exec+TCP-handshake of `curl` dominates the
# measurement, not harbor itself). The shell mode is fine as a
# "is anything broken under concurrency?" smoke; for honest perf
# numbers, install oha or wrk.
#
# Usage:
#   scripts/load-test.sh <base-url> <bearer-token>
#                        [<concurrency>] [<duration-or-reqs>]
#                        [--shell]
#
# Examples:
#   # 50 concurrent connections, 5 seconds (default)
#   scripts/load-test.sh http://127.0.0.1:9494 $TOKEN
#
#   # 100 connections, 30 seconds
#   scripts/load-test.sh http://127.0.0.1:9494 $TOKEN 100 30
#
#   # Force shell mode (no oha/wrk dependency)
#   scripts/load-test.sh http://127.0.0.1:9494 $TOKEN 8 240 --shell
#
# Pass/fail rules (applied to whichever mode runs):
#   - error rate > 0.5% → exit 1
#   - p95 > 1 s on SELECT 42 → warning (not failure; reverse-proxy /
#     container overhead can legitimately push this)

set -euo pipefail

# ----------------------------------------------------------------- args ---

if [ $# -lt 2 ]; then
  cat >&2 <<EOF
Usage: $0 <base-url> <bearer-token> [<concurrency>] [<duration-or-reqs>] [--shell]
  duration-or-reqs:
    - in tool mode (oha/wrk): seconds (default: 5)
    - in shell mode:          requests per worker (default: 30)
EOF
  exit 64
fi

BASE_URL="${1%/}"
TOKEN="$2"
CONCURRENCY="${3:-50}"
DURATION_OR_REQS="${4:-5}"
FORCE_SHELL=false
for a in "$@"; do
  [[ "$a" == "--shell" ]] && FORCE_SHELL=true
done

red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
blue()   { printf '\033[34m%s\033[0m\n' "$*"; }

# --------------------------------------------------------- tool detection -

TOOL=""
if ! $FORCE_SHELL; then
  if command -v oha >/dev/null 2>&1; then
    TOOL=oha
  elif command -v wrk >/dev/null 2>&1; then
    TOOL=wrk
  fi
fi

# ------------------------------------------------------------ run modes ---

run_oha() {
  local secs="$DURATION_OR_REQS"
  blue "Mode: oha — $CONCURRENCY connections, ${secs}s sustained load"
  blue "Endpoint: $BASE_URL/sql  (POST SELECT 42)"
  echo

  # oha errors with "invalid value '1' for '--no-color'" if the
  # NO_COLOR env var is set to anything other than "true"/"false".
  # Pass it through as a literal bool flag.
  local color_flag=()
  if [ "${NO_COLOR:-}" = "1" ] || [ "${NO_COLOR:-}" = "true" ]; then
    color_flag=(--no-color true)
  fi

  oha \
    "${color_flag[@]}" \
    --no-tui \
    -z "${secs}s" \
    -c "$CONCURRENCY" \
    -m POST \
    -H "Authorization: Bearer $TOKEN" \
    -H 'Content-Type: application/json' \
    -d '{"sql":"SELECT 42 AS x"}' \
    "$BASE_URL/sql" > /tmp/.harbor-load-out 2>&1

  cat /tmp/.harbor-load-out

  # Parse for pass/fail.
  local rps err_rate p95
  rps=$(awk '/^[[:space:]]*Requests\/sec:/ {print $2; exit}' /tmp/.harbor-load-out)
  err_rate=$(awk '/^[[:space:]]*Success rate:/ {sub("%","",$3); printf "%.4f", (100 - $3) / 100; exit}' /tmp/.harbor-load-out)
  p95=$(awk '/95.* in / {print $3; exit}' /tmp/.harbor-load-out)

  echo
  blue "Parsed:  rps=$rps  err_rate=${err_rate:-?}  p95=${p95:-?} secs"
  apply_pass_fail "${err_rate:-0}" "${p95:-0}"
}

run_wrk() {
  local secs="$DURATION_OR_REQS"
  blue "Mode: wrk — $CONCURRENCY connections, ${secs}s sustained load"
  blue "Endpoint: $BASE_URL/sql  (POST SELECT 42)"
  echo

  # wrk needs a Lua script for POST bodies + custom headers.
  local script
  script="$(mktemp)"
  trap 'rm -f "$script"' EXIT
  cat > "$script" <<EOF
wrk.method  = "POST"
wrk.body    = '{"sql":"SELECT 42 AS x"}'
wrk.headers["Authorization"] = "Bearer $TOKEN"
wrk.headers["Content-Type"]  = "application/json"
EOF

  wrk -t 4 -c "$CONCURRENCY" -d "${secs}s" \
      --latency -s "$script" "$BASE_URL/sql" \
      > /tmp/.harbor-load-out 2>&1
  cat /tmp/.harbor-load-out

  # wrk's stdout has a Latency Distribution table when --latency is set.
  local rps err non2xx
  rps=$(awk '/Requests\/sec:/ { gsub(",", "", $2); print $2; exit }' /tmp/.harbor-load-out)
  non2xx=$(awk '/Non-2xx or 3xx responses:/ { gsub(",", "", $4); print $4; exit }' /tmp/.harbor-load-out)
  non2xx="${non2xx:-0}"
  local total
  total=$(awk '/[0-9]+ requests in / { gsub(",", "", $1); print $1; exit }' /tmp/.harbor-load-out)
  total="${total:-1}"
  err=$(awk -v n="$non2xx" -v t="$total" 'BEGIN { printf "%.4f", n / t }')
  local p95
  p95=$(awk '/^[[:space:]]*99%/ { print $2; exit }' /tmp/.harbor-load-out)
  # Convert wrk's "1.71ms"/"1.71s" suffix into seconds for our threshold.
  local p95_s
  p95_s=$(awk -v v="$p95" 'BEGIN {
    if (v ~ /us$/) { sub("us","",v); printf "%f", v / 1e6 }
    else if (v ~ /ms$/) { sub("ms","",v); printf "%f", v / 1e3 }
    else if (v ~ /s$/) { sub("s","",v); printf "%f", v + 0 }
    else { printf "%s", v }
  }')

  echo
  blue "Parsed:  rps=$rps  err_rate=$err  p99=$p95 (≈${p95_s} s)"
  apply_pass_fail "$err" "$p95_s"
}

run_shell() {
  local per_worker="$DURATION_OR_REQS"
  yellow "==============================================================="
  yellow "  SHELL MODE — pure-curl loop, no real benchmarker installed."
  yellow "  These numbers are FAR slower than reality because each curl"
  yellow "  is a fresh process + TCP handshake + 2 timestamp subshells."
  yellow "  Install oha or wrk for an honest measurement:"
  yellow "    macOS:   brew install oha"
  yellow "    Ubuntu:  apt-get install wrk"
  yellow "    cargo:   cargo install oha"
  yellow "==============================================================="
  echo

  local total=$((CONCURRENCY * per_worker))
  blue "Mode: shell — $CONCURRENCY workers × $per_worker requests = $total requests"
  blue "Endpoint: $BASE_URL/sql  (POST SELECT 42)"
  echo

  local out_dir
  out_dir="$(mktemp -d)"
  trap 'rm -rf "$out_dir"' EXIT

  worker() {
    local id="$1" out="$2"
    for i in $(seq 1 "$per_worker"); do
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

  local START END
  START=$(date +%s.%N)
  for w in $(seq 1 "$CONCURRENCY"); do
    worker "$w" "$out_dir/w$w.tsv" &
  done
  wait
  END=$(date +%s.%N)

  cat "$out_dir"/w*.tsv > "$out_dir/all.tsv"
  local elapsed rows rps ok_count err_count
  elapsed=$(awk -v s="$START" -v e="$END" 'BEGIN { printf "%.3f", e - s }')
  rows=$(wc -l < "$out_dir/all.tsv" | tr -d ' ')
  rps=$(awk -v r="$rows" -v e="$elapsed" 'BEGIN { printf "%.1f", r / e }')
  ok_count=$(awk -F'\t' '$3 == 200 { c++ } END { print c+0 }' "$out_dir/all.tsv")
  err_count=$((rows - ok_count))

  local latencies median p95 p99 max
  latencies=$(awk -F'\t' '{ print $4 }' "$out_dir/all.tsv" | sort -n)
  median=$(echo "$latencies" | awk -v r="$rows" 'NR == int(r/2) + 1 { print; exit }')
  p95=$(echo "$latencies" | awk -v r="$rows" 'NR == int(r * 0.95) + 1 { print; exit }')
  p99=$(echo "$latencies" | awk -v r="$rows" 'NR == int(r * 0.99) + 1 { print; exit }')
  max=$(echo "$latencies" | tail -1)

  printf "  total requests        : %d\n" "$rows"
  printf "  elapsed wall-clock    : %s s\n" "$elapsed"
  printf "  throughput            : %s req/s (shell-bound; install oha for real numbers)\n" "$rps"
  printf "  HTTP 200 / errors     : %d / %d\n" "$ok_count" "$err_count"
  printf "  latency median        : %.3f s\n" "$median"
  printf "  latency p95           : %.3f s\n" "$p95"
  printf "  latency p99           : %.3f s\n" "$p99"
  printf "  latency max           : %.3f s\n" "$max"
  echo

  local err_rate
  err_rate=$(awk -v e="$err_count" -v t="$rows" 'BEGIN { printf "%.4f", e / t }')
  apply_pass_fail "$err_rate" "$p95"
}

apply_pass_fail() {
  local err_rate="$1" p95="$2"
  if awk -v er="$err_rate" 'BEGIN { exit !(er > 0.005) }'; then
    red "FAIL: error rate $err_rate (> 0.5%)"
    exit 1
  else
    green "OK: error rate $err_rate (≤ 0.5%)"
  fi
  if awk -v p="$p95" 'BEGIN { exit !(p + 0 > 1.0) }'; then
    yellow "WARN: p95 latency $p s (> 1s on SELECT 42 — investigate)"
  fi
}

# -------------------------------------------------------- dispatch --------

case "$TOOL" in
  oha)   run_oha ;;
  wrk)   run_wrk ;;
  *)     run_shell ;;
esac
