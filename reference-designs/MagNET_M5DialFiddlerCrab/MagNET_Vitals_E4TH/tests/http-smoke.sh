#!/usr/bin/env bash
#
# http-smoke.sh — exercise every Phase 3 HTTP endpoint on a flashed MagNET
# Vitals device and report pass/fail per the test-plan.md procedure IDs.
#
# Usage:
#   ./tests/http-smoke.sh <ip>
#   ./tests/http-smoke.sh <ip> -v      # verbose: print response bodies on fail
#   DEV=192.168.1.42 ./tests/http-smoke.sh
#
# Exit code: 0 if all procedures pass, 1 if any fail.
#
# Covers test-plan.md §7.1, §7.2, §7.4. Cross-validation, soak, and the
# negative paths that need physical interaction (cover the lux sensor,
# unplug the radar, drop WiFi mid-fetch) stay manual.

set -u

DEV="${1:-${DEV:-}}"
VERBOSE=0
[[ "${2:-}" == "-v" || "${1:-}" == "-v" ]] && VERBOSE=1

if [[ -z "$DEV" || "$DEV" == "-v" ]]; then
    echo "usage: $0 <device-ip> [-v]"
    echo "       DEV=<ip> $0 [-v]"
    exit 2
fi

# ─── Tooling check ──────────────────────────────────────────────────

for tool in curl jq; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing tool: $tool" >&2
        exit 2
    fi
done

# ─── ANSI colour (TTY-only) ─────────────────────────────────────────

if [[ -t 1 ]]; then
    C_OK=$'\033[32m'; C_FAIL=$'\033[31m'; C_DIM=$'\033[2m'; C_BOLD=$'\033[1m'; C_OFF=$'\033[0m'
else
    C_OK=''; C_FAIL=''; C_DIM=''; C_BOLD=''; C_OFF=''
fi

PASS_COUNT=0
FAIL_COUNT=0
FAILED_IDS=()

pass() {
    local id="$1" msg="${2:-}"
    printf "  %s%-28s%s %sPASS%s  %s\n" "$C_BOLD" "$id" "$C_OFF" "$C_OK" "$C_OFF" "$msg"
    PASS_COUNT=$((PASS_COUNT + 1))
}

fail() {
    local id="$1" msg="$2" body="${3:-}"
    printf "  %s%-28s%s %sFAIL%s  %s\n" "$C_BOLD" "$id" "$C_OFF" "$C_FAIL" "$C_OFF" "$msg"
    if [[ "$VERBOSE" == "1" && -n "$body" ]]; then
        printf "    %s%s%s\n" "$C_DIM" "${body:0:300}" "$C_OFF"
    fi
    FAIL_COUNT=$((FAIL_COUNT + 1))
    FAILED_IDS+=("$id")
}

# ─── HTTP helpers ───────────────────────────────────────────────────

# get_body <path>  — captures body (stdout) and HTTP status (return code).
# On non-2xx: still prints body, returns 1.
get_body() {
    local path="$1"
    local out status
    out=$(curl -sS --connect-timeout 5 --max-time 10 -w '\n%{http_code}' "http://$DEV$path") || return 2
    status="${out##*$'\n'}"
    out="${out%$'\n'*}"
    printf "%s" "$out"
    [[ "$status" == "200" ]]
}

# get_headers <method> <path>  — prints headers + status line; return = HTTP/2xx
get_headers() {
    curl -sSI -X "$1" --connect-timeout 5 --max-time 10 "http://$DEV$2" 2>/dev/null
}

# Test that the body parses as JSON.
expect_json() {
    local id="$1" path="$2"
    local body
    body=$(get_body "$path") || { fail "$id" "HTTP non-200 on $path" "$body"; return 1; }
    if echo "$body" | jq -e . >/dev/null 2>&1; then
        pass "$id" "$path → valid JSON"
        return 0
    else
        fail "$id" "$path → invalid JSON" "$body"
        return 1
    fi
}

# Test that all named keys are present in the JSON object.
expect_keys() {
    local id="$1" path="$2" shift_done=0; shift; shift
    local body keys=("$@")
    body=$(get_body "$path") || { fail "$id" "HTTP non-200" "$body"; return 1; }
    for k in "${keys[@]}"; do
        if ! echo "$body" | jq -e "has(\"$k\")" >/dev/null 2>&1; then
            fail "$id" "$path missing key '$k'" "$body"
            return 1
        fi
    done
    pass "$id" "$path has [${keys[*]}]"
}

# Run a jq filter and assert exit-0 with non-empty output.
expect_jq() {
    local id="$1" path="$2" filter="$3" descr="$4"
    local body
    body=$(get_body "$path") || { fail "$id" "HTTP non-200" "$body"; return 1; }
    if echo "$body" | jq -e "$filter" >/dev/null 2>&1; then
        pass "$id" "$descr"
    else
        fail "$id" "$descr" "$body"
    fi
}

# ─── Procedures ─────────────────────────────────────────────────────

echo
echo "${C_BOLD}MagNET Vitals HTTP smoke — http://$DEV/${C_OFF}"
echo

# Reachability
if ! curl -sf --connect-timeout 3 --max-time 5 "http://$DEV/vitals" >/dev/null 2>&1; then
    echo "${C_FAIL}device unreachable at http://$DEV/vitals${C_OFF}"
    echo "  • is the device on the same network?"
    echo "  • does \`prov-status\` at the REPL show CONNECTED with this IP?"
    exit 1
fi

echo "${C_BOLD}§7.1 per-endpoint smoke${C_OFF}"

expect_keys "P3-HTTP-VITALS-01" "/vitals" \
    bpm rpm presence distance_cm range_flag lux \
    total_phase breath_phase heart_phase target_count \
    fw_version timestamp_us

expect_keys "P3-HTTP-HR-01" "/heart-rate" bpm presence timestamp_us

expect_jq   "P3-HTTP-HRH-01"  "/heart-rate/history" \
    '.samples | type == "array"' \
    "/heart-rate/history → samples array"

expect_jq   "P3-HTTP-HRH-02"  "/heart-rate/history" \
    '.samples | (length == 0) or (. as $s | range(1; length) | $s[.] .t > $s[.-1].t)' \
    "/heart-rate/history → t strictly increasing"

expect_keys "P3-HTTP-BR-01"  "/breathing"  rpm timestamp_us
expect_jq   "P3-HTTP-BRH-01" "/breathing/history" \
    '.samples | type == "array"' \
    "/breathing/history → samples array"

expect_keys "P3-HTTP-PRES-01" "/presence" present distance_cm age_ms timestamp_us

expect_jq   "P3-HTTP-LUX-01"  "/lux" \
    '(.lux | type) == "number" or (.lux == null and (.error // "") | length > 0)' \
    "/lux → number or {null,error}"

expect_keys "P3-HTTP-TGT-01"  "/targets" count targets
expect_jq   "P3-HTTP-TGT-02"  "/targets" \
    '.count == (.targets | length)' \
    "/targets → count matches array length"

expect_jq   "P3-HTTP-PHASES-01" "/phases" \
    'length == 3' \
    "/phases → exactly 3 channels"

expect_jq   "P3-HTTP-PHASES-02" "/phases" \
    '[.[] | length] | unique | length == 1' \
    "/phases → all channels equal length"

expect_jq   "P3-HTTP-PHASES-03" "/phases" \
    '[.[] | length] | unique[0] <= 200' \
    "/phases → ≤ 200 samples per channel"

echo
echo "${C_BOLD}§7.2 CORS preflight (wildcard OPTIONS)${C_OFF}"

cors_check() {
    local id="$1" path="$2"
    local headers
    headers=$(get_headers "OPTIONS" "$path") || { fail "$id" "OPTIONS $path connect failed"; return; }
    local status="$(printf '%s' "$headers" | head -1 | tr -d '\r')"
    local origin="$(printf '%s' "$headers" | grep -i '^access-control-allow-origin:' | head -1 | tr -d '\r')"
    local methods="$(printf '%s' "$headers" | grep -i '^access-control-allow-methods:' | head -1 | tr -d '\r')"
    local ok=1
    [[ "$status" =~ 204 ]] || ok=0
    [[ "$origin" =~ \*  ]] || ok=0
    [[ "$methods" =~ GET ]] || ok=0
    if [[ "$ok" == "1" ]]; then
        pass "$id" "OPTIONS $path → 204 + CORS headers"
    else
        fail "$id" "OPTIONS $path → status='$status' origin='$origin' methods='$methods'" "$headers"
    fi
}

cors_check "P3-CORS-01" "/vitals"
cors_check "P3-CORS-02a" "/heart-rate"
cors_check "P3-CORS-02b" "/phases"
cors_check "P3-CORS-02c" "/targets"
cors_check "P3-CORS-02d" "/some/random/path"   # wildcard OPTIONS should still 204

# CORS on GET responses too
get_origin_check() {
    local id="$1" path="$2"
    local headers
    headers=$(get_headers "GET" "$path") || { fail "$id" "GET headers"; return; }
    if printf '%s' "$headers" | grep -qi '^access-control-allow-origin:.*\*'; then
        pass "$id" "GET $path → CORS header present"
    else
        fail "$id" "GET $path → missing Access-Control-Allow-Origin: *" "$headers"
    fi
}
get_origin_check "P3-CORS-03" "/vitals"

echo
echo "${C_BOLD}§7.4 JSON robustness across every endpoint${C_OFF}"

ALL_ENDPOINTS=( /vitals /heart-rate /breathing /presence /lux /targets /phases
                /heart-rate/history /breathing/history )

ALL_OK=1
for ep in "${ALL_ENDPOINTS[@]}"; do
    body=$(get_body "$ep") || { ALL_OK=0; continue; }
    if ! echo "$body" | jq -e . >/dev/null 2>&1; then
        fail "P3-JSON-01:$ep" "$ep returned invalid JSON" "$body"
        ALL_OK=0
    fi
done
[[ "$ALL_OK" == "1" ]] && pass "P3-JSON-01" "every endpoint returns valid JSON"

# /vitals .lux must be number or null — never string/object
expect_jq "P3-JSON-02" "/vitals" \
    '(.lux | type) == "number" or (.lux | type) == "null"' \
    "/vitals .lux is number or null"

# ─── Summary ────────────────────────────────────────────────────────

echo
total=$((PASS_COUNT + FAIL_COUNT))
if [[ "$FAIL_COUNT" == "0" ]]; then
    echo "${C_OK}${C_BOLD}all $total checks passed${C_OFF}"
    exit 0
else
    echo "${C_FAIL}${C_BOLD}$FAIL_COUNT/$total failed${C_OFF}"
    echo "  failed IDs:"
    for id in "${FAILED_IDS[@]}"; do
        echo "    - $id"
    done
    echo "  re-run with -v to see response bodies on failure"
    exit 1
fi
