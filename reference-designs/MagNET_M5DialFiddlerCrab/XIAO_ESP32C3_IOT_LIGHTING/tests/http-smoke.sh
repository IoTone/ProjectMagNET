#!/usr/bin/env bash
# Smoke test for the UC2 neopixel actuator API.
#
#   ./http-smoke.sh [host]
#
# host defaults to magnet-lighting.local; pass the device IP if mDNS
# isn't resolving (e.g. ./http-smoke.sh 192.168.1.42).
#
# Exercises the exact contract the in-XR UC2 actuator panel uses:
#   GET  /api/v1/actuator/neopixel
#   POST /api/v1/actuator/neopixel  { on,brightness_pct,color,pattern,pattern_speed_pct }
#   OPTIONS (CORS preflight)

set -u
HOST="${1:-magnet-lighting.local}"
BASE="http://${HOST}/api/v1/actuator/neopixel"
fail=0

req() {  # method body expect_substr label
  local method="$1" body="$2" expect="$3" label="$4" out
  if [ -n "$body" ]; then
    out=$(curl -sS --connect-timeout 5 --max-time 10 \
          -X "$method" -H 'Content-Type: application/json' \
          -d "$body" "$BASE")
  else
    out=$(curl -sS --connect-timeout 5 --max-time 10 -X "$method" "$BASE")
  fi
  if printf '%s' "$out" | grep -q "$expect"; then
    echo "PASS  $label"
  else
    echo "FAIL  $label"
    echo "      got: $out"
    fail=1
  fi
}

echo "== neopixel API @ $BASE =="

req GET  ""                                              '"available_patterns"' "GET returns state"
req POST '{"on":true}'                                   '"on":true'            "POST on=true"
req POST '{"brightness_pct":40}'                         '"brightness_pct":40'  "POST brightness"
req POST '{"color":{"r":255,"g":0,"b":0}}'               '"r":255'              "POST color red"
req POST '{"pattern":"rainbow"}'                          '"pattern":"rainbow"'  "POST pattern rainbow"
req POST '{"pattern":"breathing","pattern_speed_pct":75}' '"pattern_speed_pct":75' "POST pattern+speed"
req POST '{"pattern":"bogus"}'                            '"pattern"'            "POST bad pattern ignored (still valid JSON)"
req POST '{"on":false}'                                   '"on":false'           "POST off"

# CORS preflight: expect 204 + Access-Control-Allow-Origin
echo "== CORS preflight =="
hdrs=$(curl -sS -I -X OPTIONS --connect-timeout 5 --max-time 10 "$BASE")
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
