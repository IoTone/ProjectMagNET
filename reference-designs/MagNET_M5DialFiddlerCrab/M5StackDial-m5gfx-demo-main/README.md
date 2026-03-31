# M5Stack Dial Quick M5GFX Demo (PlatformIO, not ESP-IDF, not LVGL)

Here's a quick interactive playground for the M5Stack Dial round display which uses the S3 stamp. Rotate the dial to change brightness, tap for an expanding circular 'ping', drag a crosshair showing X,Y, press BtnA to change colours, long press for a full screen 'starbust', and long‑press the screen to invert colours.

It’s C++ (not MicroPython), built with PlatformIO using the Arduino (not ESP-IDF) framework on ESP32‑S3, and uses M5GFX directly (not LVGL). Libraries used are M5Dial, M5Unified, M5GFX. These seem, to me, some of the main technical design choices you face with the M5Stack Dial, and my other repos will explore the other options.

![M5Stack Dial demo](docs/assets/m5stack-demo.gif)

I made this because the manufacturer libraries and docs seem either too much or too little.

The outer ring is a 'poker chip' style I 3D printed (available on request).

## Purpose

This repo is a hands‑on demo of the core I/O and rendering paths on the Dial (StampS3). Minimal code in C++, no LVGL:

- Touch
  - Tap (with movement threshold) → visual ping + debug log
  - Drag (live X,Y, crosshair overlay)
  - Long press (screen) → invert colours
- Sound
  - Directional click tones on rotary up/down
  - Two‑tone sounds
  - Effect tones (starburst)
- Rotary encoder
  - Accumulator → logical detents (default 10% per step)
  - Event logging and click feedback
- Screen brightness
  - Brightness mapped from 0–100% → 0–255 via `Display.setBrightness`
- Rendering patterns
  - Watch‑face ticks from the physical edge inward
  - Theme‑driven colors (primary/text/accent/ripple)
  - Small‑area snapshot/restore (`readRect`/`pushImage`) overlays to avoid full redraws
- Buttons
  - BtnA press: cycle theme; hold: starburst effect
- Debug & Config
  - Event‑based logs (PRESS/DRAG/RELEASE, ROT, BTN, effects)
  - All tunables in `Config` at the top of `src/main.cpp`

## Controls

- Rotate: brightness (0–100%), watch‑face ticks and big % readout
- Tap: ping (outline shockwave)
- Drag: crosshair follows finger; live X/Y
- BtnA press: cycle colours/theme
- BtnA hold: starburst effect (full‑screen, two‑tone sound)
- Long press (screen): invert display (two‑tone be‑boop)

## Libraries and architecture

- M5Unified — common HAL/shim for input, display, and audio across M5 devices
- M5GFX — fast graphics backend used by M5Unified’s `Display`
- M5Dial — glue for the StampS3 Dial hardware that exposes `Display`, `Touch`, `Encoder`, `Speaker`

No LVGL
- I may add LVGL later but first wanted a simple version. Direct M5GFX calls have lower overhead and avoid additional buffers/complexity. Code stays small and easy to tweak.

How it works:
- Display: `M5Dial.Display` (from M5Unified/M5GFX) for all drawing (lines, circles, text).
- Touch: `M5Dial.Touch` with a tiny state machine to detect tap/drag/long‑press reliably.
- Encoder: `M5Dial.Encoder.readAndReset()` → small accumulator → 10% logical steps, with up/down click tones.
- Speaker: `M5Dial.Speaker.tone(freq, ms)`; a simple timer schedules two‑tone sounds.
- Overlays: small `readRect`/`pushImage` snapshots for crosshair/ping to avoid full‑frame redraws and flicker.

PlatformIO deps (from `platformio.ini`):

```
lib_deps =
  m5stack/M5Dial
  m5stack/M5Unified
  m5stack/M5GFX
```

## Tooling and environment

This project is written in C++ and targets the Arduino framework on ESP32‑S3, using PlatformIO + VS Code for builds and dependency management.

- Use PlatformIO in VS Code as configured in `platformio.ini`.
- Language: C++ (Arduino API via M5Unified/M5GFX)
- Not Arduino IDE: this repo expects PlatformIO’s dependency resolver, multiple envs, and reproducible builds.
- Not MicroPython: this demo leans on fast per‑frame drawing (lines/circles, small overlay blits) and precise input timing; CPython on ESP32S3 adds overhead and different input/audio APIs.
- Not UIFlow 2.0/LVGL: I found UIFlow 2.0 unreliable, slow and locked in. Here we draw directly with M5GFX for lower latency and simpler control.

Why PlatformIO?
- Reproducible: pins exact Espressif platform/toolchains and M5 libraries.
- Multi‑env: dev versus release builds (debug printf on/off) with a simple flag.
- Easy onboarding: open the folder in VS Code, click PlatformIO Build/Upload/Monitor.

## Flash prebuilt binary

  `esptool.py --chip esp32s3 --baud 921600 write_flash -z 0x0 merged-firmware.bin`

## Or build yourself

- Dev build (debug on): `pio run -e dev`
- Release build (debug off): `pio run -e release`
- Upload (dev): `pio run -e dev -t upload`
- Monitor: `pio device monitor -b 115200`

## Event logs to serial

- `[ROT] +10% -> br=70%`
- `[TOUCH] PRESS x=.. y=..`
- `[TOUCH] DRAG start d2=..`
- `[TOUCH] RELEASE dur=.. TAP -> ping`
- `[TOUCH] RELEASE dur=.. invert (no-drag)`
- `[TOUCH] RELEASE dur=.. drag refresh`
- `[BTN] A press -> theme N`
- `[BTN] A hold -> starburst`
- `[EFFECT] Starburst start/end`
- `[PING] end`

## Keywords to find

M5Stack Dial, StampS3, ESP32‑S3, round display, PlatformIO, VS Code, Arduino C++, M5Unified, M5GFX, rotary encoder, touch (tap/drag/long‑press), screen brightness, demo.

---

Have fun.
