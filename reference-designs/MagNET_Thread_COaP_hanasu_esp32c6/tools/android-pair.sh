#!/usr/bin/env bash
# android-pair.sh — accept a BLE pairing request on an Android device over adb.
#
# Why this exists: a bench run drives the phone through `flutter run`, so the
# tester cannot also be watching for the pairing prompt. Android gives the
# request ~30 s and then silently drops it (BOND_STATE_BONDING ->
# BOND_STATE_NONE, exactly 30.0 s later); the node just sees
# `enc_change status=13`. Unattended, every run fails the same way.
#
#   tools/android-pair.sh <adb-serial> [timeout-secs] [dump-dir]
#
# Exit 0 once the device reports BOND_STATE_BONDED, 1 otherwise.
#
# The request surfaces in one of two shapes and we handle both:
#   * a dialog activity (com.android.settings BluetoothPairingDialog), or
#   * a shade notification whose action button is only rendered once the
#     notification *itself* is expanded — expanding the shade is not enough,
#     which is exactly why the first version of this script never worked.
#
# Every hierarchy seen is written to the dump dir, so a failed run leaves
# evidence to read instead of a shrug.
set -uo pipefail

ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
SERIAL="${1:?usage: android-pair.sh <adb-serial> [timeout-secs] [dump-dir]}"
TIMEOUT="${2:-120}"
DUMPDIR="${3:-/tmp/magnet-pair-$$}"
mkdir -p "$DUMPDIR"

sh() { "$ADB" -s "$SERIAL" shell "$@" 2>/dev/null; }

ui() {
  sh uiautomator dump /sdcard/ui.xml >/dev/null 2>&1
  sh cat /sdcard/ui.xml
}

# Centre ("x y") of the first node whose text/content-desc matches $1 (an ERE),
# read out of the hierarchy in $2. Empty output means no match.
centre_of() {
  local pat="$1" xml="$2"
  printf '%s' "$xml" | tr '<' '\n' \
    | grep -iE "(text|content-desc)=\"[^\"]*(${pat})[^\"]*\"" \
    | grep -oE 'bounds="\[[0-9]+,[0-9]+\]\[[0-9]+,[0-9]+\]"' | head -1 \
    | grep -oE '[0-9]+' | tr '\n' ' ' \
    | awk 'NF==4 { print int(($1+$3)/2), int(($2+$4)/2) }'
}

bond_state() {
  sh dumpsys bluetooth_manager \
    | grep -oE 'BOND_STATE_(NONE|BONDING|BONDED)' | tail -1
}

# en-US labels plus the common ja ones, so a locale switch does not silently
# break this the way a single hardcoded string would.
PAIR_BTN='Pair & connect|^Pair$|ペア設定する|ペア設定'
REQ_TEXT='pairing request|Tap to pair|ペア設定のリクエスト'

echo "watching up to ${TIMEOUT}s for a pairing request (dumps: $DUMPDIR)"
deadline=$(( $(date +%s) + TIMEOUT ))
tick=0

while [ "$(date +%s)" -lt "$deadline" ]; do
  tick=$((tick + 1))
  if [ "$(bond_state)" = "BOND_STATE_BONDED" ]; then
    echo "bonded."
    exit 0
  fi

  xml=$(ui)
  printf '%s' "$xml" > "$DUMPDIR/ui-$tick.xml"

  # 1. A real dialog is the easy case — its button is already on screen.
  xy=$(centre_of "$PAIR_BTN" "$xml")
  if [ -n "$xy" ]; then
    echo "tapping pair button at $xy (dialog)"
    sh input tap $xy
    sleep 3
    continue
  fi

  # 2. Otherwise the notification. Open the shade, expand the row itself, then
  #    re-dump — the action button does not exist in the hierarchy until then.
  if printf '%s' "$xml" | grep -qiE "$REQ_TEXT" \
     || sh dumpsys notification --noredact | grep -qiE "$REQ_TEXT"; then
    echo "pairing request present — expanding"
    sh cmd statusbar expand-notifications >/dev/null
    sleep 2
    xml=$(ui)

    row=$(centre_of "$REQ_TEXT" "$xml")
    if [ -n "$row" ]; then
      set -- $row
      # Swipe down *on the row* to expand it. A tap would fire the row's own
      # content intent (which opens Settings) instead of revealing the action.
      sh input swipe "$1" "$2" "$1" $(( $2 + 260 )) 220
      sleep 2
      xml=$(ui)
      printf '%s' "$xml" > "$DUMPDIR/ui-$tick-expanded.xml"
    fi

    xy=$(centre_of "$PAIR_BTN" "$xml")
    if [ -n "$xy" ]; then
      echo "tapping pair button at $xy (notification)"
      sh input tap $xy
      sleep 3
      sh cmd statusbar collapse >/dev/null
      continue
    fi
    echo "request visible but no actionable Pair button yet" >&2
  fi

  sleep 2
done

echo "no bond within ${TIMEOUT}s (last state: $(bond_state)); dumps in $DUMPDIR" >&2
exit 1
