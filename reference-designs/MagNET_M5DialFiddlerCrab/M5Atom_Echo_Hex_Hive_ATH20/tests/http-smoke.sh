#!/usr/bin/env bash
# Smoke test for the UC2 environment sensor API.
#
#   ./http-smoke.sh <host>
#
# host = device IP, or the MAC-suffixed mDNS hostname once the hive flow
# publishes it. Examples:
#   ./http-smoke.sh 10.0.0.144
#   ./http-smoke.sh magnet-atom-echo-b7c0.local
#
# Exercises the contract the UC2 dataspace will hit via Vite proxy:
#   GET /api/v1/sensor/environment   -> {temperature_c, humidity_pct, ts_ms, ...}
#   GET /api/v1/sensor/temperature   -> {value_c, ts_ms}
#   GET /api/v1/sensor/humidity      -> {value_pct, ts_ms}
#   OPTIONS (CORS preflight)

set -u
HOST="${1:-10.0.0.144}"
BASE="http://${HOST}/api/v1/sensor"
fail=0

req() {  # url-suffix expect_substr label
  local suffix="$1" expect="$2" label="$3" out
  out=$(curl -sS --connect-timeout 5 --max-time 10 "${BASE}${suffix}")
  if printf '%s' "$out" | grep -q "$expect"; then
    echo "PASS  $label"
  else
    echo "FAIL  $label"
    echo "      got: $out"
    fail=1
  fi
}

echo "== env API @ $BASE =="

req /environment '"temperature_c"' "GET environment"
req /environment '"humidity_pct"'  "GET environment has humidity"
req /environment '"calibration"'   "GET environment has calibration block"
req /temperature '"value_c"'       "GET temperature"
req /humidity    '"value_pct"'     "GET humidity"

echo "== CORS preflight =="
hdrs=$(curl -sS -I -X OPTIONS --connect-timeout 5 --max-time 10 "$BASE/environment")
if printf '%s' "$hdrs" | grep -qi '204' && \
   printf '%s' "$hdrs" | grep -qi 'access-control-allow-origin'; then
  echo "PASS  OPTIONS 204 + CORS headers"
else
  echo "FAIL  OPTIONS preflight"
  echo "$hdrs"
  fail=1
fi

echo
[ "$fail" -eq 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
