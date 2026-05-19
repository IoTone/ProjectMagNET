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

| Logical strip | GPIO | Original sketch |
|---------------|------|-----------------|
| 0 | 0 | strip1 |
| 1 | 5 | strip2 |
| 2 | 1 | strip3 |
| 3 | 2 | strip4 |

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

## Phase 2 (next)

BLE→WiFi provisioning (`craw_ble_provision` / `craw_wifi`), mDNS, and an HTTP
API matching UC2's `/api/v1/actuator/neopixel` shape (on/off, brightness_pct,
color{r,g,b}, pattern, pattern_speed_pct), modeled on `MagNET_Vitals_E4TH`.
The bundled `components/craw_*` and `led_strip` managed dep are kept in place
for that phase even though Phase 1 does not link them.
