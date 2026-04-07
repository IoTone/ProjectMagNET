#!/usr/bin/env bash
# claude-crawdad-hook.sh — Multi-session Claude Code hook for Crawdad display
#
# Publishes state to a per-session MQTT topic so the Crawdad dashboard
# can track multiple concurrent Claude CLI sessions.
#
# Topic format: iotj/cl/openwr/updates/<device_mac4>/<session_id>
#
# Compatible with both:
#   - M5StickC Plus Crawdad (subscribes to .../mac4/# wildcard, shows all sessions)
#   - M5Dial Fiddler Crab (subscribes to .../mac4, also receives these messages)
#
# Required tools: jq, mosquitto_pub
#   brew install jq mosquitto
#
# Environment variables:
#   CLAW_BROKER  — MQTT broker hostname (default: broker.hivemq.com)
#   CLAW_TOPIC   — Base MQTT topic (required, e.g. iotj/cl/openwr/updates/b7a4)
#   CLAW_PLAN    — Plan tier token limit per 5hr window (default: 80000)
#                  Rough estimates: Pro=45000, Max5=80000, Max20=200000
#
# MQTT message format: state|model|session_pct|weekly_pct|reset_epoch|client_host
# Published to: CLAW_TOPIC/session_id

set -euo pipefail

CLAW_BROKER="${CLAW_BROKER:-broker.hivemq.com}"
CLAW_TOPIC="${CLAW_TOPIC:-}"
CLAW_PLAN="${CLAW_PLAN:-80000}"
CLIENT_HOST="$(hostname -s)"
CACHE_DIR="${HOME}/.claude/.craw_cache"

# Bail if no topic configured
if [ -z "$CLAW_TOPIC" ]; then
    exit 0
fi

mkdir -p "$CACHE_DIR"

# Read hook JSON from stdin
INPUT="$(cat)"

# Extract fields
HOOK_EVENT="$(echo "$INPUT" | jq -r '.hook_event_name // empty' 2>/dev/null)"
MODEL_RAW="$(echo "$INPUT" | jq -r '.model // empty' 2>/dev/null)"
TRANSCRIPT="$(echo "$INPUT" | jq -r '.transcript_path // empty' 2>/dev/null)"
SESSION_ID="$(echo "$INPUT" | jq -r '.session_id // "unknown"' 2>/dev/null)"

# --- Model name: cache per session ---
SESSION_CACHE="${CACHE_DIR}/${SESSION_ID}"
mkdir -p "$SESSION_CACHE" 2>/dev/null || true

if [ -n "$MODEL_RAW" ] && [ "$MODEL_RAW" != "null" ]; then
    SHORT_MODEL="${MODEL_RAW#claude-}"
    echo "$SHORT_MODEL" > "$SESSION_CACHE/model"
elif [ -f "$SESSION_CACHE/model" ]; then
    SHORT_MODEL="$(cat "$SESSION_CACHE/model")"
else
    SHORT_MODEL="unknown"
fi

# --- Map event to state ---
case "${HOOK_EVENT}" in
    PreToolUse)        STATE=2 ;;
    PostToolUse)       STATE=2 ;;
    Stop)              STATE=5 ;;
    Notification)      STATE=3 ;;
    UserPromptSubmit)  STATE=0 ;;
    SessionStart)      STATE=0 ;;
    *)                 STATE=0 ;;
esac

# --- Session usage % ---
SESSION_PCT=-1

if [ "$HOOK_EVENT" = "Stop" ] || [ "$HOOK_EVENT" = "UserPromptSubmit" ]; then
    if [ -n "$TRANSCRIPT" ] && [ -f "$TRANSCRIPT" ]; then
        TOTAL_OUTPUT="$(jq -s '[.[] | select(.type=="assistant") | .message.usage.output_tokens // 0] | add // 0' "$TRANSCRIPT" 2>/dev/null)" || TOTAL_OUTPUT=0

        if [ "$CLAW_PLAN" -gt 0 ] && [ "$TOTAL_OUTPUT" -gt 0 ]; then
            SESSION_PCT=$(( TOTAL_OUTPUT * 100 / CLAW_PLAN ))
            if [ "$SESSION_PCT" -gt 100 ]; then
                SESSION_PCT=100
            fi
        else
            SESSION_PCT=0
        fi
        echo "$SESSION_PCT" > "$SESSION_CACHE/session_pct"
    fi
else
    if [ -f "$SESSION_CACHE/session_pct" ]; then
        SESSION_PCT="$(cat "$SESSION_CACHE/session_pct")"
    fi
fi

# Weekly usage: not available from Claude Code yet
WEEKLY_PCT=-1
RESET_EPOCH=0

# --- Publish to per-session topic ---
MSG="${STATE}|${SHORT_MODEL}|${SESSION_PCT}|${WEEKLY_PCT}|${RESET_EPOCH}|${CLIENT_HOST}"
mosquitto_pub -h "$CLAW_BROKER" -t "${CLAW_TOPIC}/${SESSION_ID}" -q 1 -m "$MSG" 2>/dev/null || true
