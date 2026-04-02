#!/usr/bin/env bash
# claude-claw-hook.sh — Publish Claude Code state to Fiddler Crab display via MQTT
#
# Usage: Called by Claude Code hooks. Reads hook JSON from stdin.
#
# Required tools: jq, mosquitto_pub
#   brew install jq mosquitto
#
# Environment variables (set defaults, override as needed):
#   CLAW_BROKER  — MQTT broker hostname (default: broker.hivemq.com)
#   CLAW_TOPIC   — MQTT topic (default: must be set, e.g. iotj/cl/openwr/updates/b7a4)
#   CLAW_PLAN    — Plan tier token limit per 5hr window (default: 80000)
#                  Rough estimates: Pro=45000, Max5=80000, Max20=200000
#
# MQTT message format: state|model|session_pct|weekly_pct|reset_epoch|client_host

set -euo pipefail

CLAW_BROKER="${CLAW_BROKER:-broker.hivemq.com}"
CLAW_TOPIC="${CLAW_TOPIC:-}"
CLAW_PLAN="${CLAW_PLAN:-80000}"
CLIENT_HOST="$(hostname -s)"
CACHE_DIR="${HOME}/.claude/.claw_cache"

# Bail if no topic configured
if [ -z "$CLAW_TOPIC" ]; then
    exit 0
fi

# Ensure cache directory exists
mkdir -p "$CACHE_DIR"

# Read hook JSON from stdin
INPUT="$(cat)"

# Extract fields from hook JSON
HOOK_EVENT="$(echo "$INPUT" | jq -r '.hook_event_name // empty' 2>/dev/null)"
MODEL_RAW="$(echo "$INPUT" | jq -r '.model // empty' 2>/dev/null)"
TRANSCRIPT="$(echo "$INPUT" | jq -r '.transcript_path // empty' 2>/dev/null)"

# Model name: cache it when available (Stop and SessionStart include it,
# PreToolUse and others don't). Read from cache when not in JSON.
if [ -n "$MODEL_RAW" ] && [ "$MODEL_RAW" != "null" ]; then
    SHORT_MODEL="${MODEL_RAW#claude-}"
    echo "$SHORT_MODEL" > "$CACHE_DIR/model"
else
    # Read from cache
    if [ -f "$CACHE_DIR/model" ]; then
        SHORT_MODEL="$(cat "$CACHE_DIR/model")"
    else
        SHORT_MODEL="unknown"
    fi
fi

# Map hook event to state
case "${HOOK_EVENT}" in
    PreToolUse)        STATE=2 ;;
    PostToolUse)       STATE=2 ;;
    Stop)              STATE=5 ;;
    Notification)      STATE=3 ;;
    UserPromptSubmit)  STATE=0 ;;
    SessionStart)      STATE=0 ;;
    *)                 STATE=0 ;;
esac

# Compute session usage % from transcript (if available)
SESSION_PCT=-1
if [ -n "$TRANSCRIPT" ] && [ -f "$TRANSCRIPT" ]; then
    # Sum output tokens from assistant messages (output tokens are the primary billing metric)
    TOTAL_OUTPUT="$(jq -s '[.[] | select(.type=="assistant") | .message.usage.output_tokens // 0] | add // 0' "$TRANSCRIPT" 2>/dev/null)" || TOTAL_OUTPUT=0

    if [ "$CLAW_PLAN" -gt 0 ] && [ "$TOTAL_OUTPUT" -gt 0 ]; then
        SESSION_PCT=$(( TOTAL_OUTPUT * 100 / CLAW_PLAN ))
        if [ "$SESSION_PCT" -gt 100 ]; then
            SESSION_PCT=100
        fi
    fi
    # Cache session_pct for hooks that don't have transcript_path
    echo "$SESSION_PCT" > "$CACHE_DIR/session_pct"
else
    # Read cached session_pct
    if [ -f "$CACHE_DIR/session_pct" ]; then
        SESSION_PCT="$(cat "$CACHE_DIR/session_pct")"
    fi
fi

# Weekly usage: not available yet (-1)
WEEKLY_PCT=-1
RESET_EPOCH=0

# Build and send MQTT message
MSG="${STATE}|${SHORT_MODEL}|${SESSION_PCT}|${WEEKLY_PCT}|${RESET_EPOCH}|${CLIENT_HOST}"
mosquitto_pub -h "$CLAW_BROKER" -t "$CLAW_TOPIC" -m "$MSG" 2>/dev/null || true
