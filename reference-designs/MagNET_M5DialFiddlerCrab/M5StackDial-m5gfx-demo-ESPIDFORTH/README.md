# M5StackDial-m5gfx-demo-ESPIDFORTH

Phase 3 of the MagNET Hive AI prototype: the M5Stack Dial playground demo ported from Arduino to ESP-IDF with ESPIDFORTH integration.

Uses M5GFX (LovyanGFX) natively on ESP-IDF for the round 240x240 GC9A01 display and FT5x06 touch, ESP-IDF PCNT for the rotary encoder, LEDC for the buzzer, and GPIO for the button. The Forth REPL runs in a background task over USB-serial-JTAG.

## Hardware

- **Board**: M5Stack Dial (ESP32-S3, 8MB flash, no PSRAM)
- **Display**: GC9A01 240x240 round IPS LCD (SPI)
- **Touch**: FT5x06 capacitive (I2C)
- **Encoder**: Rotary quadrature on GPIO 40/41
- **Speaker**: Buzzer on GPIO 3 (LEDC PWM)
- **Button A**: GPIO 42 (dial press)

## Features

All features from the original Arduino demo, running on pure ESP-IDF:

- **Ring scale**: 100-tick brightness indicator around the edge (watch-face style)
- **Rotary encoder**: Adjusts brightness 0-100% in 10% steps with click feedback
- **Touch crosshair**: Drag to move, with live X/Y coordinate display
- **Tap ping**: Expanding circle ripple animation at touch point
- **Long-press invert**: Toggle display color inversion
- **Button themes**: Press to cycle through 6 color themes (Carbon, Neon, Ember, Ocean, Lime, Sunset)
- **Button starburst**: Hold for expanding/retracting ray animation
- **Speaker feedback**: Click, confirm, pop, invert, and starburst tones
- **Forth REPL**: Interactive control over USB-serial-JTAG

## Build and Flash

```bash
pio run -e esp32s3 -t upload -t monitor
```

## Forth REPL Commands

In addition to all standard ESPIDFORTH words (see `ESPIDFORTH/README.md`), this project registers:

| Command | Stack | Description |
|---------|-------|-------------|
| `N theme` | ( n -- ) | Set color theme 0-5 (Carbon, Neon, Ember, Ocean, Lime, Sunset) |
| `N brightness` | ( n -- ) | Set brightness 0-100% |
| `ping` | ( -- ) | Trigger ping ripple animation at screen center |
| `starburst` | ( -- ) | Trigger starburst ray animation |
| `invert` | ( -- ) | Toggle display color inversion |
| `mute` | ( -- ) | Toggle audio mute |
| `theme?` | ( -- n ) | Push current theme index |
| `bright?` | ( -- n ) | Push current brightness percent |
| `appbeep` | ( -- ) | Play a short 2kHz beep (ignores mute) |
| `appsleep` | ( -- ) | Blank display + drop backlight to 0. Wake on touch, encoder, or button |
| `appshowmem` | ( -- ) | Full-screen overlay: free heap / internal / min-free / largest block / uptime. Exit on any input |
| `appdevinfo` | ( -- ) | Full-screen overlay: chip model + rev, cores, flash size, MAC, IDF / Forth versions. Exit on any input |

### Examples

```
ok> 3 theme
ok> 50 brightness
ok> ping
ok> starburst
ok> invert
ok> mute
ok> theme? .
3
ok> bright? .
50
ok>
```

## Color Themes

| # | Name | Primary | Accent |
|---|------|---------|--------|
| 0 | Carbon | Cyan | White |
| 1 | Neon | Green | Magenta |
| 2 | Ember | Orange | Red |
| 3 | Ocean | Blue | Light Blue |
| 4 | Lime | Yellow-Green | White |
| 5 | Sunset | Yellow | Red |

## Architecture

```
┌──────────────────┐     ┌──────────────────┐
│   Main Loop      │     │  Forth REPL Task  │
│   (rendering)    │     │  (background)     │
│                  │     │                   │
│  encoder poll    │◄───►│  theme            │
│  touch poll      │     │  brightness       │
│  button poll     │     │  ping             │
│  speaker update  │     │  starburst        │
│  ring draw       │     │  invert / mute    │
│  ping anim       │     │                   │
│  crosshair       │     │  forth_repl()     │
│  starburst       │     │                   │
└────────┬─────────┘     └────────┬──────────┘
         │                        │
    ┌────┴────┐              USB-serial-JTAG
    │  M5GFX  │
    │  PCNT   │
    │  LEDC   │
    │  GPIO   │
    └─────────┘
```

## What Changed from the Original

| Original (Arduino) | This Port (ESP-IDF) |
|--------------------|--------------------|
| `M5Dial.begin()` | `LGFX_M5Dial display; display.init()` |
| `M5Dial.Display.*` | `display.*` (same M5GFX API) |
| `M5Dial.Touch.*` | `display.getTouch()` |
| `M5Dial.Encoder.readAndReset()` | ESP-IDF PCNT on GPIO 40/41 |
| `M5Dial.Speaker.tone()` | LEDC PWM on GPIO 3 |
| `M5Dial.BtnA.wasPressed()` | GPIO 42 polling with debounce |
| `millis()` | `esp_timer_get_time() / 1000` |
| `delay()` | `vTaskDelay()` |
| (none) | ESPIDFORTH REPL with 8 FFI words |

## Project Structure

```
M5StackDial-m5gfx-demo-ESPIDFORTH/
  platformio.ini                # PlatformIO config (espidf, S3 no-PSRAM board)
  CMakeLists.txt                # Top-level CMake
  sdkconfig.defaults            # Common ESP-IDF config
  sdkconfig.defaults.esp32s3    # S3-specific (no PSRAM, USB console)
  sdkconfig.esp32s3             # PIO env config (disables PSRAM)
  src/
    CMakeLists.txt              # Main component
    idf_component.yml           # M5GFX dependency
    main.cpp                    # Full demo + Forth FFI (~870 lines)
  components/
    forth/                      # ESPIDFORTH component (v0.2.0)
```

## Memory Usage

| Metric | Value |
|--------|-------|
| Flash | 339 KB (32.3% of 1 MB) |
| Static RAM | 73 KB (22.4% of 328 KB) |
| Forth heap | 64 KB |
