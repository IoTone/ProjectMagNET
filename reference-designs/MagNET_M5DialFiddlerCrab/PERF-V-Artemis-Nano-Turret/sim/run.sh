#!/bin/sh
# Simulate the turret RTL. Needs iverilog (brew install icarus-verilog).
# No Vivado, no Windows, no board: this is the part that runs on the Mac.
set -e
cd "$(dirname "$0")"
command -v iverilog >/dev/null || { echo "iverilog not found: brew install icarus-verilog" >&2; exit 1; }
command -v vvp >/dev/null      || { echo "vvp not found (ships with iverilog)" >&2; exit 1; }

work=$(mktemp -d) || { echo "could not make a work directory" >&2; exit 1; }
trap 'rm -rf "$work"' EXIT HUP INT TERM

rc=0
for tb in tb_step_gen tb_motion_axis tb_sweep; do
	echo "=== $tb"
	if ! iverilog -g2001 -Wall -o "$work/$tb.vvp" "$tb.v" ../rtl/*.v; then
		echo "FAILED to compile $tb.v" >&2
		rc=1
		continue
	fi
	out=$(vvp "$work/$tb.vvp") || { echo "$out"; echo "FAILED to run $tb" >&2; rc=1; continue; }
	echo "$out"
	echo "$out" | grep -q "ALL PASS" || rc=1
done

if [ $rc -eq 0 ]; then
	echo "--- all testbenches passed"
else
	echo "--- FAILURES" >&2
fi
exit $rc
