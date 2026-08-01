#!/usr/bin/env bash
# android-pair.sh — accept a pending BLE pairing request on an Android device.
#
# Android surfaces a BLE pairing request as a *notification* ("Tap to pair
# with …"), not as a dialog, so nothing ever takes window focus and an
# unattended test just watches SMP time out (enc_change status=13,
# BLE_HS_ETIMEOUT). This finds that notification, opens the shade and taps
# the action, so a hands-off bench run can bond.
#
#   tools/android-pair.sh <adb-serial> [timeout-secs]
#
# Exit 0 when a request was found and tapped, 1 if none appeared in time.
set -euo pipefail

ADB="${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}"
SERIAL="${1:?usage: android-pair.sh <adb-serial> [timeout-secs]}"
TIMEOUT="${2:-90}"

sh() { "$ADB" -s "$SERIAL" shell "$@"; }

echo "waiting up to ${TIMEOUT}s for a pairing request..."
deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
  if sh dumpsys notification --noredact 2>/dev/null | grep -qi 'pairing request'; then
    target=$(sh dumpsys notification --noredact 2>/dev/null \
             | grep -oiE 'Tap to pair with [^.]*' | head -1)
    echo "pairing request found: ${target:-unknown device}"

    # The action lives in the notification shade; open it and tap the button.
    sh cmd statusbar expand-notifications >/dev/null 2>&1 || true
    sleep 2
    sh uiautomator dump /sdcard/ui.xml >/dev/null 2>&1 || true

    # The notification is usually collapsed, so the "Pair & connect" action
    # is not rendered — only the row. Match the row by its content-desc (which
    # is locale-independent on this handset) and fall back to the action label.
    bounds=$(sh cat /sdcard/ui.xml 2>/dev/null | tr '>' '\n' \
      | grep -iE 'content-desc="[^"]*Pairing request[^"]*"|text="(Pair &amp; connect|ペア設定( する)?)"' \
      | grep -oE 'bounds="\[[0-9]+,[0-9]+\]\[[0-9]+,[0-9]+\]"' | head -1 \
      | grep -oE '[0-9]+' | tr '\n' ' ')

    if [ -n "$bounds" ]; then
      set -- $bounds
      x=$(( ($1 + $3) / 2 )); y=$(( ($2 + $4) / 2 ))
      echo "tapping pairing notification at ${x},${y}"
      sh input tap "$x" "$y"
      sleep 3
      # A confirm dialog may follow the row tap; accept it if present.
      sh uiautomator dump /sdcard/ui.xml >/dev/null 2>&1 || true
      cb=$(sh cat /sdcard/ui.xml 2>/dev/null | tr '>' '\n' \
        | grep -iE 'text="(Pair|ペア設定する|OK)"' \
        | grep -oE 'bounds="\[[0-9]+,[0-9]+\]\[[0-9]+,[0-9]+\]"' | head -1 \
        | grep -oE '[0-9]+' | tr '\n' ' ')
      if [ -n "$cb" ]; then
        set -- $cb
        echo "confirming dialog at $(( ($1+$3)/2 )),$(( ($2+$4)/2 ))"
        sh input tap $(( ($1+$3)/2 )) $(( ($2+$4)/2 ))
        sleep 2
      fi
      sh cmd statusbar collapse >/dev/null 2>&1 || true
      exit 0
    fi

    echo "could not locate the Pair button; leaving the shade open for a human" >&2
    exit 1
  fi
  sleep 2
done

echo "no pairing request appeared within ${TIMEOUT}s" >&2
exit 1
