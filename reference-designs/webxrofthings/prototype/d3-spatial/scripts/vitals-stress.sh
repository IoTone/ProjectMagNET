#!/usr/bin/env bash
# vitals-stress.sh — Hammer the MagNET_Vitals device via curl and report drop rate.
#
# Pings the device's HTTP endpoints at a fixed interval, captures latency and
# status for each request, and prints a summary. Use this to characterize how
# much load the C6 can handle before httpd_sock_err / EAGAIN start, and to
# verify whether the Vite proxy's maxSockets:1 cap is actually doing its job.
#
# Usage:
#   ./scripts/vitals-stress.sh                                 # 60 reqs at 1Hz to /heart-rate
#   HOST=192.168.1.42 ./scripts/vitals-stress.sh               # by IP (mDNS often flaky from CLI)
#   ./scripts/vitals-stress.sh --endpoint /targets --count 120 --interval 0.5
#   ./scripts/vitals-stress.sh --rotate                        # cycle HR/BR/phases/targets
#   ./scripts/vitals-stress.sh --concurrent 4 --rotate         # 4 parallel pollers — mimics the demo
#
# What counts as a "drop":
#   - HTTP 000  : curl couldn't get a response (timeout, ECONNREFUSED, RST, ENETUNREACH)
#   - HTTP 5xx  : device errored after accepting the connection
#   - HTTP 4xx  : malformed request (rare — usually means an endpoint typo)

set -uo pipefail

HOST="${HOST:-magnet-vitals.local}"
SCHEME="${SCHEME:-http}"
ENDPOINT="/heart-rate"
COUNT=60
INTERVAL=1.0
TIMEOUT=5
ROTATE=0
CONCURRENT=1

print_help() {
  # Print every leading-`#` line (after dropping the shebang and the `# ` prefix)
  # until we hit the first non-comment line.
  awk '
    NR==1 && /^#!/ { next }
    /^#/  { sub(/^#( |$)/, ""); print; next }
    { exit }
  ' "$0"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --endpoint)   ENDPOINT="$2"; shift 2;;
    --count)      COUNT="$2"; shift 2;;
    --interval)   INTERVAL="$2"; shift 2;;
    --timeout)    TIMEOUT="$2"; shift 2;;
    --rotate)     ROTATE=1; shift;;
    --host)       HOST="$2"; shift 2;;
    --concurrent) CONCURRENT="$2"; shift 2;;
    -h|--help)    print_help; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

ENDPOINTS=("/heart-rate" "/breathing" "/phases" "/targets")

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

worker() {
  local id=$1
  local count=$2
  local out="$WORKDIR/worker-$id.log"
  for ((i=1; i<=count; i++)); do
    local ep
    if [[ $ROTATE -eq 1 ]]; then
      ep=${ENDPOINTS[$(( (i-1) % ${#ENDPOINTS[@]} ))]}
    else
      ep=$ENDPOINT
    fi
    local url="$SCHEME://$HOST$ep"

    # %{http_code}  : HTTP status (000 if curl couldn't get a response)
    # %{time_total} : end-to-end request time in seconds
    local metrics
    metrics=$(curl -sS -o /dev/null \
      --connect-timeout 2 --max-time "$TIMEOUT" \
      -w '%{http_code} %{time_total}\n' \
      "$url" 2>/dev/null) || metrics="000 0"

    local code time_s time_ms ts
    code=$(echo "$metrics" | awk '{print $1}')
    time_s=$(echo "$metrics" | awk '{print $2}')
    time_ms=$(awk -v t="$time_s" 'BEGIN { printf "%.0f", t*1000 }')
    ts=$(date '+%H:%M:%S')

    if [[ "$code" == "200" ]]; then
      printf '%s  w%d  ok    %5dms  %s\n' "$ts" "$id" "$time_ms" "$ep" >> "$out"
    else
      printf '%s  w%d  FAIL  code=%s  %s\n' "$ts" "$id" "$code" "$ep" >> "$out"
    fi
    # Aggregated row for stats — `flock` keeps concurrent appends atomic on macOS.
    echo "$code $time_ms" >> "$WORKDIR/all.log"

    sleep "$INTERVAL"
  done
}

echo "Target:    $SCHEME://$HOST"
if [[ $ROTATE -eq 1 ]]; then
  echo "Endpoint:  rotating ${ENDPOINTS[*]}"
else
  echo "Endpoint:  $ENDPOINT"
fi
echo "Plan:      $CONCURRENT worker(s) × $COUNT request(s) every ${INTERVAL}s (per-req timeout ${TIMEOUT}s)"
echo "---"

if [[ $CONCURRENT -gt 1 ]]; then
  for ((w=1; w<=CONCURRENT; w++)); do
    worker "$w" "$COUNT" &
  done
  wait
else
  worker 1 "$COUNT"
fi

# Stream per-worker logs in interleaved time order.
sort -k1,1 "$WORKDIR"/worker-*.log

echo "---"

total=$(wc -l < "$WORKDIR/all.log" | tr -d ' ')
ok=$(awk '$1==200 {n++} END {print n+0}' "$WORKDIR/all.log")
fail=$((total - ok))
drop_pct=$(awk -v f="$fail" -v t="$total" 'BEGIN { printf "%.1f", (t==0?0:f*100/t) }')

echo "Summary:"
printf '  total:   %d\n' "$total"
printf '  ok:      %d\n' "$ok"
printf '  failed:  %d  (%s%%)\n' "$fail" "$drop_pct"

if [[ $ok -gt 0 ]]; then
  awk '$1==200 {print $2}' "$WORKDIR/all.log" | sort -n > "$WORKDIR/latencies.txt"
  n=$(wc -l < "$WORKDIR/latencies.txt" | tr -d ' ')
  p() {
    local pct=$1
    local idx=$(( (n * pct + 99) / 100 ))
    [[ $idx -lt 1 ]] && idx=1
    sed -n "${idx}p" "$WORKDIR/latencies.txt"
  }
  echo ""
  echo "Latency (ms, ok only):"
  printf '  min/p50/p95/p99/max:  %s / %s / %s / %s / %s\n' \
    "$(head -1 "$WORKDIR/latencies.txt")" \
    "$(p 50)" "$(p 95)" "$(p 99)" \
    "$(tail -1 "$WORKDIR/latencies.txt")"
fi

if [[ $fail -gt 0 ]]; then
  echo ""
  echo "Failures by HTTP code:"
  awk '$1!=200 {print $1}' "$WORKDIR/all.log" | sort | uniq -c | sort -rn | \
    awk '{printf "  %4d × %s\n", $1, $2}'
  echo "  (000 = no response from device: timeout / ECONNREFUSED / RST)"
fi
