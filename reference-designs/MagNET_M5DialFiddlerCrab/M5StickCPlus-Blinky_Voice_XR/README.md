# M5StickC Plus — ROBOTALK XR (Voice + IMU)

A voice/IMU XR node on the M5StickC Plus for the AXR Worlds Engine. Forked from
`M5StickCPlus-Blinky_Crawdad_OpenWR` (which monitored Claude Code sessions) — this
build drops session tracking and instead captures microphone audio on demand and
streams live IMU orientation over MQTT, with an animated robot face on a 135x240
synthwave display.

## Quick Start

### 1. Flash

```bash
cd M5StickCPlus-Blinky_Voice_XR
pio run -e m5stick_c_plus -t upload -t monitor
```

### 2. Configure WiFi (via REPL at 115200 baud)

```
ok> wifi-ssid
SSID: YourNetwork
ok> wifi-pass
Password: ********
ok> wifi-connect
```

### 3. Calibrate the IMU

On first WiFi connect the device prompts for a flat-on-table calibration
(set it level and still for ~2 s). Re-run anytime with `imu-cal` at the REPL,
button **A** on the CONVO screen, or clear the saved calibration from Settings.

### 4. Get your MQTT topic

```
ok> mqtt-topic
iotj/axrawe/updates/a1b2
```

Live IMU telemetry is published here at ~10 Hz once calibrated + connected:

```json
{"id":"a1b2","ts":123456789,"ax":0.01,"ay":-0.02,"az":9.79,
 "gx":0.001,"gy":0.000,"gz":-0.002,"roll":0.3,"pitch":-0.1,"yaw":2.4}
```

A short "voice event" is also published after each mic capture:

```json
{"id":"a1b2","evt":"voice","dur_s":4.00,"peak":18234}
```

## Screens

### MAIN — "ROBOTALK XR" (default)
Connection info, MQTT + IMU status, heap, profile, battery. An animated robot
face fidgets (blinks / glances) occasionally while WiFi is connected.

### CONVO
- **Mic**: hold **B** to record. Capture is buffered in internal DRAM (no PSRAM
  on this board) at 16 kHz / 16-bit mono. Shows a fill bar while recording and
  the last capture's duration + peak level afterward.
- **IMU**: live roll / pitch / yaw (deg) + raw accel, plus calibration status.

### Settings (layout unchanged)
Sound on/off, brightness, profile cycle, **Clear** (now clears IMU calibration),
Back. Button A cycles items, Button B toggles.

## Audio buffer length

No PSRAM, so the record buffer comes from internal DRAM. At 16 kHz/16-bit mono
(32 KB/s) the firmware sizes the buffer against the largest free block at record
time and caps it at **`MIC_MAX_RECORD_SECS` = 4 s** (~3–4 s realistic). Check the
live ceiling with `mic-secs`. Drop the rate to 8 kHz to roughly double the time,
or stream over MQTT in chunks for unbounded length.

## Button Navigation

| Button | MAIN | CONVO | Settings |
|--------|------|-------|----------|
| **A** (front) | → CONVO | Calibrate IMU | Cycle items |
| **B** (side, short) | → Settings | → MAIN | Toggle/activate |
| **B** (hold ~0.8 s) | — | **Record mic** | — |
| **B** (hold 3 s) | — | — | Clear WiFi + reboot |

## Forth commands

```
N brightness                          set brightness 0-100
wifi-ssid / wifi-pass / wifi-connect  WiFi setup
wifi-status / wifi-clear / wifi-profiles
mqtt-broker / mqtt-status / mqtt-topic
sound-on / sound-off
imu-cal       run flat calibration (place level & still)
imu-status    print IMU state + live orientation
imu-zero      reset the yaw datum
mic-secs      print the max capture length for current free heap
mic-test      record a clip from the REPL
```

## Shared + local components

Shared `craw_*` live in `../components/` (symlinked into `components/`). Two
board-local components are specific to the StickC Plus:

| Component | Purpose |
|-----------|---------|
| `craw_serial` | Serial I/O abstraction |
| `craw_speaker` | PWM tone generation + chimes |
| `craw_wifi` | WiFi STA with event callbacks |
| `craw_nvs` | NVS profiles + settings storage |
| `craw_mqtt` | MQTT client (used publisher-only here) |
| `craw_http` | HTTP server |
| `craw_mic` *(local)* | SPM1423 PDM mic via I2S PDM-RX |
| `craw_imu_mpu6886` *(local)* | MPU6886 IMU sampler + flat calibration |
| `forth` | ESPIDFORTH interpreter + REPL |

`craw_imu_mpu6886` reuses M5GFX's I2C bus (the MPU6886 at 0x68 shares the
internal bus with the AXP192 PMIC), so it must init after `display.init()`.

## Hardware

- **Board**: M5StickC Plus (ESP32-PICO-D4, 4MB flash, no PSRAM)
- **Display**: 135x240 ST7789V2 (SPI)
- **IMU**: MPU6886 6-DoF (I2C 0x68, SDA=G21 / SCL=G22) — some units ship SH200Q
- **Mic**: SPM1423 PDM (CLK=G0, DATA=G34) on I2S0
- **Buttons**: A=GPIO 37 (front), B=GPIO 39 (side)
- **Speaker**: GPIO 2 (PWM buzzer)
- **USB**: Type-C (UART, 115200 baud)

## Also See

- **M5StickCPlus-Blinky_Crawdad_OpenWR** — the Claude Code session monitor this
  was forked from (still maintained separately)
