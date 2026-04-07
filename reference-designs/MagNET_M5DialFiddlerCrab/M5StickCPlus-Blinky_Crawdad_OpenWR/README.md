# M5StickC Plus Crawdad OpenWR

Multi-session Claude Code monitoring dashboard on the M5StickC Plus. Tracks up to 8 concurrent Claude CLI sessions on a 135x240 synthwave display with animated crawdad characters.

## Quick Start

### 1. Flash

```bash
cd M5StickCPlus-Blinky_Crawdad_OpenWR
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

### 3. Get your MQTT topic

```
ok> mqtt-topic
iotj/cl/openwr/updates/a1b2
```

### 4. Install the hook script

```bash
cp hooks/claude-crawdad-hook.sh ~/.claude/hooks/
```

### 5. Configure Claude Code hooks

Edit `~/.claude/settings.json`:

```json
{
  "hooks": {
    "PreToolUse": [{"matcher": "", "hooks": [{"type": "command",
      "command": "CLAW_TOPIC='iotj/cl/openwr/updates/a1b2' ~/.claude/hooks/claude-crawdad-hook.sh"}]}],
    "Stop": [{"matcher": "", "hooks": [{"type": "command",
      "command": "CLAW_TOPIC='iotj/cl/openwr/updates/a1b2' ~/.claude/hooks/claude-crawdad-hook.sh"}]}],
    "Notification": [{"matcher": "", "hooks": [{"type": "command",
      "command": "CLAW_TOPIC='iotj/cl/openwr/updates/a1b2' ~/.claude/hooks/claude-crawdad-hook.sh"}]}],
    "UserPromptSubmit": [{"matcher": "", "hooks": [{"type": "command",
      "command": "CLAW_TOPIC='iotj/cl/openwr/updates/a1b2' ~/.claude/hooks/claude-crawdad-hook.sh"}]}]
  }
}
```

Replace `a1b2` with your device's MAC suffix from step 3.

## Screens

### WiFi Status (default)
Shows connection info, MQTT status, topic, heap, profile, session count.

### Sessions Dashboard
Each active Claude CLI session gets a row:
```
● abc1 opus W 02:15  8%
```
- Colored dot (assigned per session)
- 4-char session ID
- Model (opus/sonn/haik)
- State: W=working, F=finished, I=idle, N=need input, E=error
- Working time (mm:ss)
- Session token usage %

Active (WORKING) sessions show an animated mini crawdad. Sessions go stale after 5 minutes (dimmed) and are removed after 30 minutes of inactivity.

### Settings
Sound on/off, brightness, clear sessions. Button A cycles items, Button B toggles.

## Button Navigation

| Button | WiFi Screen | Sessions Screen | Settings Screen |
|--------|-------------|-----------------|-----------------|
| **A** (front) | → Sessions | → WiFi | Cycle items |
| **B** (side) | → Settings | → Settings | Toggle/activate |
| **B** (hold 3s) | Clear WiFi + reboot | Clear WiFi + reboot | Clear WiFi + reboot |

## Multi-Session MQTT

The hook script publishes to per-session topics:
```
iotj/cl/openwr/updates/<mac4>/<session_id>
```

The device subscribes to `iotj/cl/openwr/updates/<mac4>/#` (wildcard) and auto-discovers sessions. This is backward-compatible with the M5Dial Fiddler Crab (single-session) — both devices can share the same MQTT topic prefix.

## Shared Components

This project uses modular ESP-IDF components shared across Claw OpenWR devices:

| Component | Purpose |
|-----------|---------|
| `craw_serial` | Serial I/O abstraction (UART on ESP32, USB-JTAG on S3) |
| `craw_speaker` | PWM tone generation + state chimes |
| `craw_wifi` | WiFi STA with event callbacks |
| `craw_nvs` | NVS profiles + settings storage |
| `craw_mqtt` | MQTT client with message parser + session_id extraction |
| `craw_http` | HTTP server (/notify, /status, /ping) |
| `forth` | ESPIDFORTH interpreter + REPL |

Components live in `../components/` (symlinked into this project's `components/` directory).

## Hardware

- **Board**: M5StickC Plus (ESP32-PICO-D4, 4MB flash, no PSRAM)
- **Display**: 135x240 ST7789V2 (SPI)
- **Buttons**: A=GPIO 37 (front), B=GPIO 39 (side)
- **Speaker**: GPIO 2 (PWM buzzer)
- **Battery**: 120 mAh built-in
- **USB**: Type-C (UART, 115200 baud)

## Memory Usage

| Metric | Value |
|--------|-------|
| Flash | 977 KB (46.6% of 2MB partition) |
| RAM | 94 KB (28.5% of 328 KB) |
| Forth heap | 32 KB |

## Also See

- **M5StackDial-Blinky_Claw_OpenWR** — Single-session version for M5Stack Dial (round display)
- **M5Stamp3CU_Blinky_Claw_OpenWR** — LED-only version for M5Stamp C3U
- **ESPIDFORTH** — The Forth interpreter component
