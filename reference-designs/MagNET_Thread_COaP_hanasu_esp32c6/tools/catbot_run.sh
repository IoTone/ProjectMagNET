#!/bin/bash
# catbot_run.sh — one-command nekobot v2 session over the chat-bench bridge.
#
#     tools/catbot_run.sh [seconds] [--traffic] [--photos] [--bot <name>]
#
# Runs the whole rig and tears it down again: bridge (started only if one
# isn't already serving :8642), the SSE→TSV event pipeline, the Pop-11 bot,
# and optionally the conversation-partner and photo-turn loops. Transcript
# and mini-soak report land in tools/logs/catbot-<stamp>.log; exit 0 iff the
# run finished with zero delivery misses. Full docs: docs/NEKOBOT.md §6.
#
# Needs: PlatformIO penv python (pyserial), jq, curl, and the pop11 skill's
# popsession (override with POPSESSION=... if it lives elsewhere).
set -u

TOOLS="$(cd "$(dirname "$0")" && pwd)"
PY="${PIO_PY:-$HOME/.platformio/penv/bin/python}"
POPSESSION="${POPSESSION:-$HOME/.claude/skills/pop11/bin/popsession}"
URL="http://127.0.0.1:8642"

SECS=3600; TRAFFIC=0; PHOTOS=0; BOT=neko
while [ $# -gt 0 ]; do
    case "$1" in
        --traffic) TRAFFIC=1 ;;
        --photos)  PHOTOS=1 ;;
        --bot)     shift; BOT="$1" ;;
        *)         SECS="$1" ;;
    esac
    shift
done

for tool in jq curl; do
    command -v "$tool" >/dev/null || { echo "need $tool"; exit 2; }
done
test -x "$PY" || { echo "penv python not found at $PY"; exit 2; }
test -x "$POPSESSION" || { echo "popsession not found at $POPSESSION"; exit 2; }

STAMP=$(date +%Y%m%d-%H%M)
LOGDIR="$TOOLS/logs"; mkdir -p "$LOGDIR"
EVENTS="$LOGDIR/catbot-events-$STAMP.tsv"
BOTLOG="$LOGDIR/catbot-$STAMP.log"
PIDS=(); BRIDGE_PID=""

cleanup() {
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done
    [ -n "$BRIDGE_PID" ] && kill "$BRIDGE_PID" 2>/dev/null
}
trap cleanup EXIT

# 1. bridge: reuse a live one, else start it on every Hanasu port
if ! curl -sf -m 3 "$URL/nodes" >/dev/null 2>&1; then
    PORTS=$("$PY" "$TOOLS/hcp.py" list)
    [ -n "$PORTS" ] || { echo "no Hanasu nodes found"; exit 2; }
    echo "starting bridge on: $PORTS"
    # shellcheck disable=SC2086   # ports are one-per-line, no spaces
    "$PY" "$TOOLS/chat-bench/bridge.py" $PORTS >"$LOGDIR/bridge-$STAMP.log" 2>&1 &
    BRIDGE_PID=$!
    for _ in $(seq 30); do
        curl -sf -m 2 "$URL/nodes" >/dev/null 2>&1 && break
        sleep 1
    done
    curl -sf -m 2 "$URL/nodes" >/dev/null || { echo "bridge failed to start"; exit 2; }
else
    echo "reusing running bridge at $URL"
fi

# 2. SSE → TSV event pipeline (what the bot's cb_pump tails)
: > "$EVENTS"
curl -sN "$URL/events" | jq -Rrc --unbuffered '
  ltrimstr("data: ") | fromjson? |
  if .kind=="event" and .name=="chat" then ["chat", .ts, .node, .fields[2], .text]
  elif .kind=="info" and (.name != null) then ["info", .ts, .node, .name]
  elif .kind=="photo" then ["photo", 0, .node, .from, .name, .size, .secs]
  elif .kind=="xfer" and .phase=="fail" then ["xferfail", 0, .node, (.reason//"?")]
  else empty end | @tsv' >> "$EVENTS" &
PIDS+=($!)

# 3. optional company
if [ "$TRAFFIC" = 1 ]; then
    "$PY" "$TOOLS/catbot_traffic.py" --bot "$BOT" >"$LOGDIR/traffic-$STAMP.log" 2>&1 &
    PIDS+=($!)
fi
if [ "$PHOTOS" = 1 ]; then
    (cd "$TOOLS" && "$PY" catbot_photoloop.py --bot "$BOT") >"$LOGDIR/photos-$STAMP.log" 2>&1 &
    PIDS+=($!)
fi

# 4. the bot itself (foreground until the session ends)
"$POPSESSION" start --name catbot >/dev/null 2>&1
"$POPSESSION" send --name catbot -f "$TOOLS/catbot.p" >/dev/null || exit 2
"$POPSESSION" send --name catbot -f "$TOOLS/catbot_bridge.p" >/dev/null || exit 2
DRIVER=$(mktemp)
cat > "$DRIVER" <<EOF
catbot2_open('$EVENTS', '$BOT', '$BOTLOG');
catbot_run($SECS);
cb2_report();
EOF
echo "catbot '$BOT' running ${SECS}s — transcript: $BOTLOG"
"$POPSESSION" send --name catbot --timeout $((SECS + 600)) -f "$DRIVER"
rm -f "$DRIVER"

# 5. verdict from the bot's own report
tail -4 "$BOTLOG"
grep -q 'v2 REPORT: delivery.*misses=0' "$BOTLOG"
