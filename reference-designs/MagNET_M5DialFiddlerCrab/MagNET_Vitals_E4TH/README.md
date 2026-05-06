# MagNET Vitals E4TH

Personal-scale dataspace device — XIAO ESP32-C6 + Seeed MR60BHA2 60GHz mmWave radar (heart rate / breathing rate / multi-target presence) + BH1750 ambient light + WS2812 status LED. Will eventually run ESPIDFORTH and serve UC3 `kords-personal-health` (see [`specs/MagNET-Vitals-E4TH-proposal.md`](../../../specs/MagNET-Vitals-E4TH-proposal.md)).

**Status:** Phase 3 — radar driver + LED + lux + WiFi/BLE provisioning + ESPIDFORTH REPL + HTTP services (`/vitals`, `/heart-rate`, `/breathing`, `/presence`, `/lux`, `/targets`, `/phases`, plus history endpoints). UC3 dataspace manifest lands in Phase 4.

## Hardware

- [Seeed MR60BHA2 mmWave Heart-Rate Kit](https://wiki.seeedstudio.com/getting_started_with_mr60bha2_mmwave_kit/) (XIAO ESP32-C6 + radar pre-integrated).
- USB-C for power and console.

## Build & flash

PlatformIO:

```bash
cd MagNET_Vitals_E4TH
pio run -e esp32c6 -t upload -t monitor
```

ESP-IDF directly:

```bash
cd MagNET_Vitals_E4TH
idf.py set-target esp32c6
idf.py build flash monitor
```

## Expected output

On power-on the LED shows **solid amber** (booting) for ~3 s, then settles
to a **slow cyan pulse** (idle) until presence is detected. With a person
in the cone the LED switches to a **2 s pulse** whose hue maps the BPM
(blue-violet at 50 BPM → red at 100 BPM).

The console banner prints, then drops into a Forth REPL:

```
============================================
  MagNET Vitals E4TH — Phase 2
  WiFi+BLE provisioning · Forth REPL · vitals
============================================
BLE advertising as 'MagNET-vitals-a1b2'
Forth engine initialized.
Free heap: 168000 bytes

No stored WiFi. Connect via BLE to provision.

Forth commands:
  hr?          ( -- bpm )      heart rate
  br?          ( -- rpm )      breathing rate
  …

ESPIDFORTH v0.1.0 …
ok>
```

Quick smoke at the REPL:

```
ok> presence? if hr? . ." bpm  " br? . ." rpm" else ." nobody home" then
72 bpm  14 rpm
ok> vitals
HR:      72.4 bpm
BR:      14.2 rpm
Pres:    yes
Dist:    0.85 m  (range_flag=1)
Phase:   t=12.34  b= 3.21  h= 9.87
Targets: 1
   [0] x= 0.05 y= 0.85 dop=2 cluster=1
Lux:     250.8
FW:      0x01020304
ok>
```

If `presence?` always pushes 0, see [`components/craw_mr60bha2/README.md`](components/craw_mr60bha2/README.md) → "Verifying against your kit."

## Layout

```
MagNET_Vitals_E4TH/
├── platformio.ini                     # board = seeed_xiao_esp32c6
├── CMakeLists.txt                     # IDF top-level + EXTRA_COMPONENT_DIRS
├── sdkconfig.defaults                 # C6 target, USB-serial-JTAG console, NimBLE
├── partitions.csv                     # 2 MB factory partition
├── src/main.c                         # provisioning + Forth + vitals task
├── src/CMakeLists.txt
├── src/idf_component.yml              # led_strip + mdns
└── components/
    ├── craw_mr60bha2/                 # UART driver (Tiny Frame Interface)
    ├── craw_status_led/               # WS2812 status LED — semantic modes
    ├── craw_bh1750/                   # I²C ambient light sensor
    └── led_strip/                     # vendored from espressif/led_strip 3.0.3
```

`led_strip/` is a verbatim copy of the upstream IDF Component Manager
package (Apache 2.0). We vendor it instead of declaring it in
`idf_component.yml` because (a) PlatformIO's IDF wrapper sometimes skips
the manager's fetch step on this project and (b) we want a fresh checkout
to build without first having to build a sibling project.

The provisioning + Forth components are **shared** with the M5Stamp E4TH project
via `EXTRA_COMPONENT_DIRS` pointing at `../M5Stamp3CU_Blinky_E4TH/components/`.
No copying — same source, two firmwares. If they ever need to diverge, copy
locally at that point.

Reused components (target-agnostic):
- `forth/`            — ESPIDFORTH interpreter
- `craw_nvs/`         — flash-backed config (WiFi creds, profiles)
- `craw_wifi/`        — WiFi STA with event callback
- `craw_ble_provision/` — NimBLE GATT for over-air WiFi provisioning

## What's wired up vs. what isn't

| | Phase 3 (this commit) | Later |
|---|---|---|
| UART parser (`craw_mr60bha2`) — incl. phase ring | ✓ | |
| WS2812 status LED (`craw_status_led`) | ✓ | |
| BH1750 lux sensor (`craw_bh1750`) | ✓ | |
| WiFi STA + NVS-saved creds | ✓ | |
| BLE GATT provisioning (`MagNET-vitals-<MAC4>`) | ✓ | |
| ESPIDFORTH + vitals words | ✓ | |
| HTTP server (auto-starts on WiFi connect) | ✓ | |
| UC3 dataspace manifest + Vite proxy | | Phase 4 |
| mDNS `magnet-vitals-<MAC4>.local` | | Phase 4 (small — `mdns_init` + `mdns_hostname_set` + `mdns_service_add`) |
| Privacy banner / `show-privacy` HUD | | Phase 5 |

## HTTP endpoints

All return `application/json`, all set `Access-Control-Allow-Origin: *`.
Wildcard `OPTIONS /*` returns 204 with CORS preflight headers.

| Path | Body |
|---|---|
| `GET /vitals` | combined snapshot |
| `GET /heart-rate` | `{ bpm, presence, timestamp_us }` |
| `GET /heart-rate/history` | `{ samples: [{t, v}, …] }` — last 60 minutes, 1 sample/min |
| `GET /breathing` | `{ rpm, timestamp_us }` |
| `GET /breathing/history` | same shape as HR history |
| `GET /presence` | `{ present, distance_cm, age_ms, timestamp_us }` |
| `GET /lux` | `{ lux, timestamp_us }` (or `{ lux: null, error }` on I²C fail) |
| `GET /targets` | `{ count, targets: [{ id, x_m, y_m, dop, cluster }] }` |
| `GET /phases` | `[[heart…], [breath…], [total…]]` — streamgraph `distributions` shape, ~20 s of waveform |

The HTTP server auto-starts in the WiFi-connected callback once lwip
has an IP, and stops on disconnect. No restart needed after `prov-reset`.

### Quick smoke

After WiFi is up, find the device's IP from `prov-status` and:

```bash
DEV=$(ip)   # e.g. 192.168.1.42
curl http://$DEV/vitals | jq .
curl http://$DEV/heart-rate
curl http://$DEV/heart-rate/history | jq '.samples | length'
curl http://$DEV/targets | jq .
curl -s http://$DEV/phases | jq '.[] | length'   # all three should match
curl -i -X OPTIONS http://$DEV/vitals             # confirm CORS preflight 204
```

mDNS (`magnet-vitals-<MAC4>.local`) is not yet registered — Phase 4 wires
it up. Use IP for now; `prov-status` prints the current IP.

## BLE provisioning

Same GATT layout as the M5Stamp Blinky E4TH project — the dial-side
ruler discovers this node like any other MagNET hive member. UUIDs and
characteristic indices are documented in
[`../M5Stamp3CU_Blinky_E4TH/README.md`](../M5Stamp3CU_Blinky_E4TH/README.md#ble-provisioning-phase-4a).
At the REPL: `prov-status` shows current state, `prov-reset` clears creds
and re-advertises.

## Validating the parser without a kit

Capturing a few seconds of `idf.py monitor` output with debug logging
on (`CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y`) is the fastest sanity check:
the parser logs `unhandled type 0x...` for any frame type it doesn't
recognize, and `header cksum mismatch` / `data cksum mismatch` when
bytes look like a frame but fail validation. Persistent checksum
errors usually mean wrong baud, swapped TX/RX, or a different-flavor
mmWave board (FDA2 etc.).
