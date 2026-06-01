# MagNET Vitals E4TH

Personal-scale dataspace device — XIAO ESP32-C6 + Seeed MR60BHA2 60GHz mmWave radar (heart rate / breathing rate / multi-target presence) + BH1750 ambient light + WS2812 status LED. Runs ESPIDFORTH with vitals words and serves the UC3 `kords-personal-health` dataspace over HTTP. See [`specs/MagNET-Vitals-E4TH-proposal.md`](../../../specs/MagNET-Vitals-E4TH-proposal.md) for the full design rationale.

**Status:** Phase 3 — radar driver + LED + lux + WiFi/BLE provisioning + ESPIDFORTH REPL + HTTP services. UC3 dataspace manifest + d3-spatial wiring is Phase 4.

## Hardware

- [Seeed MR60BHA2 mmWave Heart-Rate Kit](https://wiki.seeedstudio.com/getting_started_with_mr60bha2_mmwave_kit/) — XIAO ESP32-C6 + 60 GHz radar + WS2812 RGB LED + BH1750 lux sensor on one board.
- USB-C for power and console.

## Build & flash

```bash
cd MagNET_Vitals_E4TH
pio run -e esp32c6 -t upload -t monitor
```

ESP-IDF directly:

```bash
idf.py set-target esp32c6
idf.py build flash monitor
```

The console is over USB-serial-JTAG (the same USB-C cable that powers the board).

## Boot sequence

1. **Banner** prints over USB-serial-JTAG with the configured pins:
   ```
   ============================================
     MagNET Vitals E4TH — Phase 3
     Radar · LED · lux · WiFi/BLE · Forth · HTTP
   --------------------------------------------
     radar:  UART0  rx=GPIO17  tx=GPIO16  @115200
     LED:    GPIO1  (WS2812)
     BH1750: sda=GPIO22  scl=GPIO23
   ============================================
   ```
2. **LED → solid amber** (booting).
3. Drivers init (BH1750, WS2812, UART for radar). Boot ROM prints `init ok` lines.
4. **Radar self-test** — waits up to 10 s for the first valid frame from the radar. Prints diagnostic counters and one of:
   - `RESULT: PASS` → **LED → solid green for 2.5 s**, then transitions into normal operation.
   - `RESULT: FAIL` → **LED → 4 Hz red flash, persists indefinitely** until a successful re-test.
5. NVS + WiFi + BLE provisioning init. Auto-connects to stored creds or stays advertising for BLE provisioning.
6. ESPIDFORTH initialises and registers the vitals words.
7. Forth REPL opens. The vitals task takes over LED control (only if the self-test passed).

## LED state machine

| State | Colour | Animation | When |
|---|---|---|---|
| Boot | amber | solid | first ~3 s after power-on |
| Self-test pass | **green** | solid | held ~2.5 s after the first valid frame |
| Self-test fail | **red** | 4 Hz flash | radar produced no valid frame in 10 s — **persists** until you fix it |
| Tracking (presence) | **blue** | gentle pulse, ~2 s period | someone is in the radar cone |
| Not tracking | **yellow** | slow breath, ~4 s period | radar OK, no one in the cone |
| `0 led-vitals` | off | — | manual REPL override |

A glance at the LED tells you the device's state without checking the console. The flashing-red mode is sticky: the vitals task respects the self-test result and won't overwrite the LED while the radar is unhealthy.

## Forth REPL commands

In addition to the standard ESPIDFORTH vocabulary:

| Word | Stack effect | Description |
|---|---|---|
| `hr?` | ( -- bpm ) | current heart rate, integer |
| `br?` | ( -- rpm ) | current breathing rate |
| `presence?` | ( -- 0\|1 ) | person in cone? |
| `distance?` | ( -- cm ) | distance to target, 0 if no presence |
| `lux?` | ( -- lux ) | BH1750 reading, −1 if sensor unavailable |
| `targets?` | ( -- count ) | number of detected targets |
| `vitals` | ( -- ) | print full snapshot |
| `hr-history.` | ( -- ) | print last 60 HR samples |
| `br-history.` | ( -- ) | print last 60 BR samples |
| `mr60-status` | ( -- ) | print radar fw version + last frame age |
| `mr60-diag` | ( -- ) | print parser counters (bytes, frames, cksum fails) |
| `mr60-test` | ( -- ok? ) | re-run 10 s self-test, drives LED |
| `mr60-rebind` | ( rx tx -- ) | hot-swap radar UART pins without reflashing |
| `led-vitals` | ( on/off -- ) | enable/disable LED feedback |
| `prov-status` | ( -- ) | print BLE/WiFi state and IP |
| `prov-reset` | ( -- ) | clear stored WiFi creds, re-advertise |

Composability example:

```
ok> presence? if hr? . ." bpm  " br? . ." rpm" else ." nobody home" then
72 bpm  14 rpm
```

## If the radar self-test fails

The console output tells you which failure mode you're in. The two most useful counters in `mr60-diag` are `bytes received` and `valid frames`.

| `bytes_received` | `valid_frames` | Diagnosis |
|---|---|---|
| 0 | 0 | Wrong UART pins or wrong UART port — driver isn't seeing any bytes |
| > 0 | 0 | Bytes arriving but parser rejects them — wrong baud, swapped RX/TX, or different mmWave variant |
| > 0 | > 0 | Healthy |
| > 0 | 0, with non-zero `header_cksum_fail` | Garbage on the line — likely RX/TX swapped or noise pickup |

### Recovery without reflashing

```forth
ok> mr60-diag                    ( confirm baseline counters )
ok> 16 17 mr60-rebind            ( try swapped pins from current 17/16 )
ok> mr60-test                    ( wait 10 s, watch counters )

\ if still 0 bytes, try other pin pairs:
ok>  4  5 mr60-rebind
ok> mr60-test
ok> 18 19 mr60-rebind
ok> mr60-test
```

After every `mr60-test` the LED transitions: **green** on PASS (then yellow/blue idle), **red flash** on FAIL.

Once you find the working pair, persist it:

```bash
# edit sdkconfig.defaults
CONFIG_CRAW_MR60_RX_GPIO=<rx>
CONFIG_CRAW_MR60_TX_GPIO=<tx>

# regenerate per-env config
rm sdkconfig sdkconfig.esp32c6 sdkconfig.old
pio run -e esp32c6
```

The stale `sdkconfig.esp32c6` will be re-seeded from `sdkconfig.defaults` on the next build. Confirm with `grep CRAW sdkconfig.esp32c6` before flashing.

## Layout

```
MagNET_Vitals_E4TH/
├── platformio.ini                     # board = esp32-c6-devkitc-1, 4 MB flash
├── CMakeLists.txt                     # IDF top-level + EXTRA_COMPONENT_DIRS append
├── sdkconfig.defaults                 # C6 target, USB-serial-JTAG console, NimBLE,
│                                      # CRAW_LED_GPIO=1, CRAW_MR60_*=0/17/16
├── partitions.csv                     # 2 MB factory partition
├── src/main.c                         # banner + drivers + self-test + Forth REPL
├── src/http_vitals.c                  # esp_http_server routes
├── src/CMakeLists.txt
├── src/idf_component.yml              # mdns (Phase 4)
└── components/
    ├── craw_mr60bha2/                 # UART driver + parser + diagnostics
    ├── craw_status_led/               # WS2812 driver — semantic modes
    ├── craw_bh1750/                   # I²C ambient light sensor
    └── led_strip/                     # vendored from espressif/led_strip 3.0.3
```

`led_strip/` is a verbatim copy of the upstream IDF Component Manager package (Apache 2.0). We vendor it because PlatformIO's IDF wrapper sometimes skips the manager's fetch step, and because we want a fresh checkout to build without a sibling project being built first.

The `forth/`, `craw_nvs/`, `craw_wifi/`, `craw_ble_provision/` components are **shared** with the M5Stamp E4TH project via `EXTRA_COMPONENT_DIRS` (see `CMakeLists.txt`). One source, two firmwares. List-append (not `set()`) so PIO's own `src/` injection survives.

## What's wired up vs. what isn't

| | Phase 3 (this commit) | Later |
|---|---|---|
| UART parser (`craw_mr60bha2`) — incl. phase ring + diagnostics | ✓ | |
| Power-on self-test with green/red LED feedback | ✓ | |
| Hot-swap UART pins via `mr60-rebind` | ✓ | |
| WS2812 status LED — yellow/blue tracking states | ✓ | |
| BH1750 lux sensor | ✓ | |
| WiFi STA + NVS-saved creds | ✓ | |
| BLE GATT provisioning (`MagNET-vitals-<MAC4>`) | ✓ | |
| ESPIDFORTH + 16 vitals/diag/provisioning words | ✓ | |
| HTTP server (auto-starts on WiFi connect) | ✓ | |
| mDNS — resolve `http://magnet-vitals.local/` on the LAN | ✓ | |
| UC3 dataspace manifest + Vite proxy in d3-spatial | ✓ | |
| Privacy banner / `show-privacy` HUD | | Phase 5 |

## HTTP endpoints

All return `application/json`, all set `Access-Control-Allow-Origin: *`. Wildcard `OPTIONS /*` returns 204 with CORS preflight headers.

| Path | Body |
|---|---|
| `GET /vitals` | combined snapshot — bpm, rpm, presence, distance_cm, range_flag, lux, three phase fields, target_count, fw_version, timestamp_us |
| `GET /heart-rate` | `{ bpm, presence, timestamp_us }` |
| `GET /heart-rate/history` | `{ samples: [{t, v}, …] }` — last 60 minutes, 1 sample/min |
| `GET /breathing` | `{ rpm, timestamp_us }` |
| `GET /breathing/history` | same shape as HR history |
| `GET /presence` | `{ present, distance_cm, age_ms, timestamp_us }` |
| `GET /lux` | `{ lux, timestamp_us }` or `{ lux: null, error }` on I²C fail |
| `GET /targets` | `{ count, targets: [{ id, x_m, y_m, dop, cluster }] }` |
| `GET /phases` | `[[heart…], [breath…], [total…]]` — streamgraph `distributions` shape, up to 200 samples per channel |

The HTTP server auto-starts when WiFi connects (lwip has an IP), stops on disconnect, restarts after a successful `prov-reset` → re-provision cycle.

### Quick smoke

```bash
DEV=192.168.1.42                                            # from `prov-status` at the REPL
curl -s http://$DEV/vitals | jq .
curl -s http://$DEV/heart-rate/history | jq '.samples | length'
curl -s http://$DEV/phases | jq '.[] | length'              # all three same; ≤ 200
curl -i -X OPTIONS http://$DEV/vitals                       # → 204 + CORS headers
```

For the full automated smoke run see [`tests/http-smoke.sh`](tests/http-smoke.sh).

## BLE provisioning

Same GATT layout as the M5Stamp Blinky E4TH project — the dial-side ruler discovers this node like any other MagNET hive member. UUIDs and characteristic indices are documented in [`../M5Stamp3CU_Blinky_E4TH/README.md`](../M5Stamp3CU_Blinky_E4TH/README.md#ble-provisioning-phase-4a). At the REPL:

- `prov-status` — current state, BLE name, WiFi state, SSID, IP.
- `prov-reset` — clear stored WiFi creds, re-advertise.

## Wiring into d3-spatial

See [`tests/http-smoke.sh`](tests/http-smoke.sh) and the next section.

## Validating the parser without a kit

Set `CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y` in `sdkconfig.defaults` and re-flash. The parser then logs `unhandled type 0x...` for any frame type it doesn't recognize, and `header cksum mismatch` / `data cksum mismatch` when bytes look like a frame but fail validation. Persistent checksum errors usually mean wrong baud, swapped TX/RX, or a different-flavor mmWave board (FDA2 etc.). Use `mr60-diag` from the REPL to see the same counters at runtime.
