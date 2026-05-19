# XIAO ESP32C3 IOT Lighting (E4TH)

Four WS2812/NeoPixel strips (60 LEDs each) driven by a software bit-bang on
the Xiao ESP32C3, scripted in ESPIDFORTH. A port of the campsynthesis
`4strings_random_change_adafruit_neopixel_xiaoesp32c3` Arduino sketch.

This is the actuator that will back UC2's LED-strip control. Phase 1 (this
checkpoint) is **port + self-test only** — flash it and verify all four
strips light correctly. WiFi/BLE provisioning and the
`/api/v1/actuator/neopixel` HTTP API come in Phase 2.

## Hardware

- **Board**: Seeed Xiao ESP32C3 in a Grove Shield for XIAO
- **Strips**: 4 × WS2812 (NEO_GRB, 800 kHz), 60 px each

| Logical strip | XIAO pad | GPIO | Original sketch |
|---------------|----------|------|-----------------|
| 0 | D0 | 2 | strip1 |
| 1 | D5 | 7 | strip2 |
| 2 | D1 | 3 | strip3 |
| 3 | D2 | 4 | strip4 |

The XIAO ESP32C3 does **not** break out raw GPIO 0/1 — its silkscreen pads
D0..D5 are GPIO 2,3,4,5,6,7. The original Arduino sketch's pin integers are
the XIAO Dx pad labels, not raw GPIO numbers (a bare-metal port must remap).

The ESP32-C3 has only 2 RMT TX channels / 1 SPI host, so it cannot drive 4
independent strips with the hardware `led_strip` driver. Instead a
cycle-counted bit-bang (interrupts disabled per strip, ~7 ms each) runs the
data lines, exposed to Forth as FFI words. There is no WiFi/BLE in Phase 1,
so nothing competes with the bit-bang timing.

## Build and flash

```bash
pio run -e esp32c3 -t upload -t monitor
```

## Verifying the strips

At the `ok>` prompt over USB serial:

| Command | What it checks |
|---------|----------------|
| `selftest` | bars → all-on → walk, the full sequence |
| `selftest-bars` | all strips RED, then GREEN, then BLUE — colors must match the printed labels (catches a wrong R/G/B order) |
| `selftest-allon` | all strips dim white for 3 s — every LED on every strip should light |
| `selftest-walk` | one pixel marches down each strip in turn — confirms wiring, pixel count, and direction per strip |
| `10 randrun` | 10 frames of the ported random-color pattern, then blanks |
| `ws-clear` | blank all strips |
| `strips` | print wiring + word list |

If a strip stays dark or shows wrong colors, check its GPIO/data wiring and
common ground against the table above.

## Low-level Forth words (FFI)

| Word | Stack | Description |
|------|-------|-------------|
| `ws-px` | ( strip idx r g b -- ) | set one pixel (no show) |
| `ws-fill` | ( strip r g b -- ) | fill one strip (no show) |
| `ws-fill-all` | ( r g b -- ) | fill all strips (no show) |
| `ws-rand-fill` | ( strip -- ) | per-pixel random color (no show) |
| `ws-show` | ( strip -- ) | blast one strip to the wire |
| `ws-show-all` | ( -- ) | blast all strips |
| `ws-clear` | ( -- ) | blank + show all strips |
| `rnd` | ( n -- r ) | pseudo-random in [0,n) |
| `nap` | ( ms -- ) | cooperative delay |

The high-level `selftest*` / `randrun` words are defined in Forth (loaded at
boot from a bundle in `main.c`) and can be redefined live at the REPL.

## Phase 2 — WiFi + UC2 HTTP API

BLE→WiFi provisioning (`craw_ble_provision` / `craw_wifi` / `craw_nvs`),
mDNS, and an HTTP API the in-XR UC2 actuator panel drives directly.

### Provisioning

On boot the device advertises over BLE as `MagNET-lighting-<MAC4>` and
accepts WiFi credentials (same flow as the M5_Hive_Cam). Once connected it
auto-reconnects on later boots. At the REPL:

- `prov-status` — BLE/WiFi state + current IP
- `prov-reset` — clear stored creds, re-advertise

On WiFi connect it starts mDNS (`magnet-lighting.local`) and the HTTP server.

### API

`GET` / `POST` `http://magnet-lighting.local/api/v1/actuator/neopixel`
(also reachable at `http://<ip>/...`). CORS-enabled (`*`) with an OPTIONS
preflight, so a browser-based WebXR client can call it cross-origin.

POST body — all fields optional (partial update):

```json
{ "on": true,
  "brightness_pct": 0-100,
  "color": { "r": 0-255, "g": 0-255, "b": 0-255 },
  "pattern": "solid|breathing|rainbow|chase|twinkle",
  "pattern_speed_pct": 0-100 }
```

GET and POST both return the full state:

```json
{ "on", "brightness_pct", "color":{"r","g","b"}, "pattern",
  "pattern_speed_pct", "led_count", "last_changed_at",
  "available_patterns":[...], "timestamp_us" }
```

A FreeRTOS render task draws the current state continuously (solid /
breathing / rainbow / chase / twinkle; `brightness_pct` maps to the `bri`
cap; `pattern_speed_pct` sets animation rate).

### Strip ownership (Phase 1 vs Phase 2)

The render task stays **disengaged** until the first HTTP command (or
`strip-engage`), so the Phase-1 Forth self-test still owns the strip at
boot. `strip-release` hands it back to the REPL; `strip?` prints state.

### Smoke test

```bash
tests/http-smoke.sh magnet-lighting.local      # or the device IP
```
Exercises GET/POST/OPTIONS against the live device; exits non-zero on any
failure.

### Wiring it to UC2

Point the UC2 actuator panel's neopixel `usm_service_endpoint` (or the
mock-join-server proxy target) at `http://magnet-lighting.local/api/v1`
instead of the simulated endpoint — the request/response shapes match, so
no client change is needed. Mind the small-chip HTTP serialization note
(cap the dev-proxy at `maxSockets: 1`).
