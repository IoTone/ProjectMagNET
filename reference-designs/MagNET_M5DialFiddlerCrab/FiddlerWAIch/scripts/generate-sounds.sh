#!/usr/bin/env bash
# Generate CAF chirps matching the M5StickC Plus speaker tones.
# Requires: sox (brew install sox)

set -euo pipefail

DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$DIR/FiddlerWAIch Watch App/Resources/Sounds"
mkdir -p "$OUT"

chirp() {
    # freq_hz duration_s out_name
    local freq="$1" dur="$2" name="$3"
    sox -n -r 16000 -c 1 "$OUT/$name.caf" \
        synth "$dur" sine "$freq" \
        fade t 0.006 "$dur" 0.006 \
        gain -3
}

two_tone() {
    # freq1 dur1 gap freq2 dur2 out_name
    local f1="$1" d1="$2" gap="$3" f2="$4" d2="$5" name="$6"
    local tmpdir tmp1 tmp2
    tmpdir="$(mktemp -d)"
    tmp1="$tmpdir/a.caf"
    tmp2="$tmpdir/b.caf"
    sox -n -r 16000 -c 1 "$tmp1" synth "$d1" sine "$f1" fade t 0.006 "$d1" 0.006 gain -3
    sox -n -r 16000 -c 1 "$tmp2" synth "$d2" sine "$f2" pad "$gap" 0 fade t 0.006 "$d2" 0.006 gain -3
    sox "$tmp1" "$tmp2" "$OUT/$name.caf"
    rm -rf "$tmpdir"
}

# WORKING: soft 800 Hz, 50 ms
chirp 800 0.050 chirp_working

# NEED INPUT: high 2000 Hz, 100 ms
chirp 2000 0.100 chirp_needinput

# FINISHED: ascending 1200 → 1800, 120 ms gap
two_tone 1200 0.080 0.120 1800 0.100 chirp_finished

# ERROR: descending 1000 → 600, 120 ms gap
two_tone 1000 0.080 0.120 600 0.100 chirp_error

# Swipe click: 400 Hz, 20 ms
chirp 400 0.020 click_swipe

# Connected beep: 1500 Hz, 60 ms
chirp 1500 0.060 beep_connected

# Disconnected beep: 500 Hz, 120 ms
chirp 500 0.120 beep_disconnected

echo "Generated CAF files in: $OUT"
ls -la "$OUT"
