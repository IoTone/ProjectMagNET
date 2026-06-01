# M5Stamp C3U Blinky E4TH

NeoPixel LED pattern demo on M5Stamp C3U, powered by ESPIDFORTH. Also the first consumer of the Phase-4A `craw_ble_provision` component: on boot the device advertises as `MagNET-biologic-<MAC4>` and accepts WiFi credentials over BLE GATT (R3 + R4 of the hive spec).

## Hardware

- **Board**: M5Stamp C3U (ESP32-C3, RISC-V, 4MB flash, no PSRAM)
- **LED**: SK6812 NeoPixel on GPIO 2 (1 LED)
- **Button**: GPIO 9 (active low, internal pull-up, triggers on release)

## Modes

Press the button or type `N blinky` at the REPL to switch modes:

| Mode | Name | Description |
|------|------|-------------|
| 0 | Off | LED off, no pattern |
| 1 | Random Flash | New random color every 200ms |
| 2 | Slow Breathe | Fade in/out on a randomly chosen hue, ~2s per breath |
| 3 | Fast Strobe | Random colors, 50ms on / 50ms off |
| 4 | Rainbow | Smooth HSV hue cycle, full spectrum every ~2 seconds |

## Build and Flash

```bash
pio run -e esp32c3 -t upload -t monitor
```

## Forth Commands

In addition to all standard ESPIDFORTH words, this project adds:

| Command | Stack | Description |
|---------|-------|-------------|
| `N blinky` | ( mode -- ) | Set pattern mode 0-4 |
| `modes` | ( -- ) | List all available modes |
| `mode?` | ( -- mode ) | Push current mode number onto stack |
| `R G B led-rgb` | ( r g b -- ) | Set LED to exact color (stops pattern) |
| `led-off` | ( -- ) | Turn off LED (stops pattern, same as `0 blinky`) |
| `prov-status` | ( -- ) | Print BLE / WiFi provisioning state, current IP |
| `prov-reset` | ( -- ) | Clear stored WiFi creds, restart BLE advertising |
| `cpu-temp` | ( -- ) | Print ESP32-C3 die temperature (°C) |
| `cpu-temp?` | ( -- centi-c ) | Push die temp in centi-Celsius (e.g. 4530 = 45.30 °C) |

> The ESP32-C3 has only an on-chip die sensor — accuracy is ±5–10 °C absolute, stable for relative readings. For ambient temperature, swap in a DS18B20 / BME280 / SHT4x and add a Forth word with the same name.

### Examples

```
ok> 0 blinky
Mode 0: Off

ok> 4 blinky
Mode 4: Rainbow

ok> 255 0 0 led-rgb

ok> modes
  0: Off
  1: Random Flash
  2: Slow Breathe
  3: Fast Strobe
  4: Rainbow

ok> mode? .
4
ok>
```

## Boot Output

```
============================================
  M5Stamp C3U Blinky E4TH v0.1.0
  Build: Apr  1 2026 13:00:00
  NeoPixel patterns via ESPIDFORTH
============================================
LED on GPIO 2, Button on GPIO 9
Forth engine initialized.
Free heap: 168000 bytes

Button cycles modes. Forth commands:
  N blinky    -- set mode (0-4)
  modes       -- list all modes
  R G B led-rgb -- set LED color (stops pattern)
  led-off     -- turn off LED
  mode?       -- show current mode

ESPIDFORTH v0.1.0 (build Apr  1 2026 13:00:00)
Type 'words' for vocabulary, 'bye' to exit

ok>
```

## Architecture

The LED pattern loop runs on a dedicated FreeRTOS task. The Forth REPL runs in `app_main`. Both share the USB-serial-JTAG driver for output. The button ISR fires on the rising edge (release) with 50ms software debounce. Custom Forth words (`blinky`, `led-rgb`, etc.) are registered via the `forth_register_word` API, which allows any C function to be called as a Forth primitive.

```
┌─────────────┐     ┌──────────────┐
│  LED Task   │     │  app_main    │
│  (patterns) │     │  (Forth REPL)│
│             │     │              │
│ check_button│◄───►│  blinky      │  Forth words set
│ set_led     │     │  led-rgb     │  current_mode
└──────┬──────┘     │  led-off     │
       │            └──────┬───────┘
       │  USB-serial-JTAG  │
       └───────────────────┘
       │
  ┌────┴────┐
  │ led_strip│  GPIO 2 (RMT)
  │ driver   │
  └─────────┘
```

## Adding Custom FFI Words

This project demonstrates how to extend ESPIDFORTH with project-specific Forth words using the `forth_register_word` API:

```c
#include "forth_core.h"

/* ( r g b -- ) Set LED color */
static void w_led_rgb(void) {
    int b = (int)forth_pop();
    int g = (int)forth_pop();
    int r = (int)forth_pop();
    set_led(r, g, b);
}

/* Register after forth_init(), before forth_repl() */
forth_register_word("led-rgb", w_led_rgb);
```

## ESPIDFORTH Component

This project uses the ESPIDFORTH component (copied into `components/forth/`). The component provides the Forth interpreter, REPL, and the external registration API (`forth_register_word`, `forth_push`, `forth_pop`) used by the blinky words. See `ESPIDFORTH/README.md` for the full standard word list.

## BLE Provisioning (Phase 4A)

On boot the device advertises as `MagNET-biologic-<MAC4>` (e.g. `MagNET-biologic-a1b2`). A connected controller (nRF Connect, LightBlue, or a MagNET ruler/gateway app) can discover the provisioning service:

- **Service UUID**: `4d41474e-4554-0001-0000-000000000000`
- **Characteristics** (UUID suffix = 01..06):
  - `01` device_info (read): JSON with chip, role, fw version, SSID, IP, status
  - `02` wifi_ssid (read/write): UTF-8, max 32 chars
  - `03` wifi_pass (write only): UTF-8, max 64 chars; reads return empty
  - `04` wifi_commit (write): uint8 trigger; writing `1` begins WiFi connect
  - `05` ip_address (read + notify): IPv4 string or `N/A`
  - `06` status (read + notify): uint8 — 0=idle, 1=creds-received, 2=commit-requested, 3=connecting, 4=connected, 5=failed

The commit flow persists credentials through `craw_nvs` using the standard `craw_config` namespace, so on next boot the node auto-connects without needing BLE again. Use `prov-reset` at the REPL to clear and re-advertise.

## Memory Usage

| Metric | Value |
|--------|-------|
| Flash | 914 KB (44.6% of 2 MB partition) |
| Static RAM | 92 KB (28.8% of 320 KB) |
| Forth heap | 64 KB |
| Components added | `craw_nvs`, `craw_wifi`, `craw_ble_provision` |
