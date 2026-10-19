#!/usr/bin/env bash
# setup_zephyr_ws.sh — create the Zephyr workspace used by ArtemisNano_Hive_Multisense.
#
# Pinned versions (keep in sync with README §5, phase 1):
#   Zephyr      v4.4.2   (upstream; NCS's fork drops hal_ambiq, so we don't reuse C:\ncs)
#   Zephyr SDK  1.0.1    (zephyr/SDK_VERSION at v4.4.2), arm-zephyr-eabi toolchain only
#
# Only zephyr + cmsis + cmsis_6 + hal_ambiq are fetched (west project-filter).
#
# Usage (Git Bash):  ./setup_zephyr_ws.sh [workspace_dir]   default: /c/zephyr-artemis
set -euo pipefail

WS="${1:-/c/zephyr-artemis}"
ZEPHYR_REV="v4.4.2"

mkdir -p "$WS"
cd "$WS"

if [ ! -d .venv ]; then
  python -m venv .venv
fi
PY=./.venv/Scripts/python
WEST=./.venv/Scripts/west
"$PY" -m pip install -q --upgrade pip west

if [ ! -d .west ]; then
  "$WEST" init -m https://github.com/zephyrproject-rtos/zephyr --mr "$ZEPHYR_REV" .
fi
# mbedtls + tf-psa-crypto: required by the Bluetooth host's crypto (PSA). Without them the
# BLE samples fail Kconfig ("PSA_WANT_* ... direct dependencies ... 0").
"$WEST" config manifest.project-filter -- '-.*,+cmsis,+cmsis_6,+hal_ambiq,+mbedtls,+tf-psa-crypto'
"$WEST" update --narrow -o=--depth=1

# `west packages pip --install` only prints a .cmd to run on Windows; call pip directly.
"$PY" -m pip install -q \
  -r zephyr/scripts/requirements-base.txt \
  -r zephyr/scripts/requirements-build-test.txt \
  -r zephyr/scripts/requirements-run-test.txt
# The SDK's setup.cmd needs 7z on PATH to unpack the toolchain. Without it, `west sdk install`
# installs only hosttools and silently skips arm-zephyr-eabi.
export PATH="/c/Program Files/7-Zip:$PATH"
"$WEST" sdk install --version 1.0.1 -t arm-zephyr-eabi
test -x "$HOME/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc.exe" \
  || { echo "arm-zephyr-eabi missing: run '$HOME/zephyr-sdk-1.0.1/setup.cmd /t arm-zephyr-eabi /c' with 7z on PATH"; exit 1; }

echo "Workspace ready: $WS"
echo "Activate with:   source $WS/.venv/Scripts/activate && export ZEPHYR_BASE=$WS/zephyr"
