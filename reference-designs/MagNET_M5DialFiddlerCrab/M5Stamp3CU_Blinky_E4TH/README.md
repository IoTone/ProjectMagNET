# M5Stamp C3U Blinky E4TH

NeoPixel LED pattern demo on M5Stamp C3U, powered by ESPIDFORTH.

Uses the ESP-IDF `led_strip` RMT driver (equivalent to Adafruit NeoPixel) for the onboard SK6812 RGB LED, and native GPIO with interrupt for the button. The ESPIDFORTH REPL is available over USB-serial-JTAG for interactive Forth commands alongside the running patterns.

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

## Memory Usage

| Metric | Value |
|--------|-------|
| Flash | 205 KB (19.5% of 1 MB) |
| Static RAM | 67 KB (20.4% of 328 KB) |
| Forth heap | 64 KB |
| Free heap | ~168 KB |
