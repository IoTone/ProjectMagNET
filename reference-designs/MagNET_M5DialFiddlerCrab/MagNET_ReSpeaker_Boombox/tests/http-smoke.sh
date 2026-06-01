#!/usr/bin/env bash
# Smoke test for the UC2 speaker actuator API.
#
#   ./http-smoke.sh <host>
#
# host = the device's IP (mDNS publishes magnet-boombox-XXXX.local once the
# hive flow completes, but the IP is the reliable default during bring-up).
# Examples:
#   ./http-smoke.sh 10.0.0.144
#   ./http-smoke.sh magnet-boombox-b7c0.local
#
# Exercises the exact contract the in-XR UC2 actuator panel uses:
#   GET  /api/v1/actuator/speaker          - device capabilities
#   POST /api/v1/actuator/speaker/play     - { sound_id } -> { played, at }
#   OPTIONS (CORS preflight)

set -u
HOST="${1:-10.0.0.144}"
BASE="http://${HOST}/api/v1/actuator/speaker"
fail=0

req() {  # method body url-suffix expect_substr label
  local method="$1" body="$2" suffix="$3" expect="$4" label="$5" out
  if [ -n "$body" ]; then
    out=$(curl -sS --connect-timeout 5 --max-time 10 \
          -X "$method" -H 'Content-Type: application/json' \
          -d "$body" "${BASE}${suffix}")
  else
    out=$(curl -sS --connect-timeout 5 --max-time 10 -X "$method" "${BASE}${suffix}")
  fi
  if printf '%s' "$out" | grep -q "$expect"; then
    echo "PASS  $label"
  else
    echo "FAIL  $label"
    echo "      got: $out"
    fail=1
  fi
}

echo "== speaker API @ $BASE =="

req GET  ""                            ''               '"sounds"'             "GET sounds catalog"
req POST '{"sound_id":"chime"}'        '/play'          '"played":"notify"'    "POST chime → notify"
req POST '{"sound_id":"doorbell"}'     '/play'          '"played":"alert"'     "POST doorbell → alert"
req POST '{"sound_id":"notify"}'       '/play'          '"played":"notify"'    "POST notify passthrough"
req POST '{"sound_id":"sunrise"}'      '/play'          '"played":"sunrise"'   "POST sunrise"
req POST '{"sound_id":"bogus"}'        '/play'          '"error"'              "POST unknown rejected"

echo "== CORS preflight =="
hdrs=$(curl -sS -I -X OPTIONS --connect-timeout 5 --max-time 10 "$BASE/play")
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
