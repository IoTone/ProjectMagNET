# M5StackDial Blinky Claw OpenWR

Synthwave-themed Claude Code status display on the M5Stack Dial. Shows a fiddler crab mascot, Claude CLI activity state, model name, client hostname, session/weekly usage rings, a session work timer, and WiFi status — all on a 240x240 round LCD.

Receives notifications via MQTT (cross-subnet) and HTTP (local), driven by Claude Code hooks.

## Quick Start

### 1. Flash the device

```bash
cd M5StackDial-Blinky_Claw_OpenWR
pio run -e esp32s3 -t upload -t monitor
```

### 2. Connect to WiFi (one-time, via REPL)

```
ok> wifi-ssid
SSID: YourNetwork
ok> wifi-pass
Password (Enter for open): ********
ok> wifi-connect
Saved. Connecting to 'YourNetwork'...
```

### 3. Get your MQTT topic

Note the 4-character MAC suffix shown on the display (above the WiFi icon), or type:

```
ok> mqtt-topic
iotj/cl/openwr/updates/b7a4
```

### 4. Install the hook script

```bash
mkdir -p ~/.claude/hooks
cp hooks/claude-claw-hook.sh ~/.claude/hooks/
```

Requires `jq` and `mosquitto_pub`:
```bash
brew install jq mosquitto
```

### 5. Configure Claude Code hooks

Edit `~/.claude/settings.json` (replace `b7a4` with your MAC suffix):

```json
{
  "hooks": {
    "PreToolUse": [{
      "matcher": "",
      "hooks": [{
        "type": "command",
        "command": "CLAW_TOPIC='iotj/cl/openwr/updates/b7a4' ~/.claude/hooks/claude-claw-hook.sh"
      }]
    }],
    "Stop": [{
      "matcher": "",
      "hooks": [{
        "type": "command",
        "command": "CLAW_TOPIC='iotj/cl/openwr/updates/b7a4' ~/.claude/hooks/claude-claw-hook.sh"
      }]
    }],
    "Notification": [{
      "matcher": "",
      "hooks": [{
        "type": "command",
        "command": "CLAW_TOPIC='iotj/cl/openwr/updates/b7a4' ~/.claude/hooks/claude-claw-hook.sh"
      }]
    }],
    "UserPromptSubmit": [{
      "matcher": "",
      "hooks": [{
        "type": "command",
        "command": "CLAW_TOPIC='iotj/cl/openwr/updates/b7a4' ~/.claude/hooks/claude-claw-hook.sh"
      }]
    }]
  }
}
```

### 6. Restart Claude Code and use it

The display updates automatically:
- **WORKING** (magenta pulse) — Claude is using tools
- **FINISHED** (green) — Claude finished responding
- **NEED INPUT** (pink/yellow flash) — Claude sent a notification
- **IDLE** (dim cyan) — waiting for your prompt

## Display Layout

```
         SESSION  42%
     ╱  Outer ring: session %    ╲
    │  ╱ Inner ring: weekly %  ╲  │
    │ │ ╱  Timer ring (orange)╲ │ │
    │ │ │    00:12:34          │ │ │
    │ │ │  ── WORKING ──       │ │ │
    │ │ │  (macbook-pro)       │ │ │
    │ │ │   🦀 Fiddler Crab    │ │ │
    │ │ │    opus-4-6          │ │ │
    │ │  ╲                    ╱ │ │
    │  ╲     WEEKLY  ?        ╱  │
     ╲        b7a4             ╱
      ╲      📶 MyNetwork     ╱
```

- **Outer ring**: Session token usage % (cyan/yellow/red gradient, 100 ticks)
- **Inner ring**: Weekly usage % (same gradient, 50 ticks)
- **Timer ring**: Active working time (orange, 60 ticks = 60 min, wraps)
- **Timer text**: `hh:mm:ss` above status, pulses when running
- **Status text**: IDLE / WORKING / FINISHED / NEED INPUT / ERROR
- **Client hostname**: In parens below status — which machine is running Claude
- **Fiddler crab**: Synthwave mascot (cyan body, magenta claws, green eyes)
- **Model name**: Below crab — "opus-4-6", "sonnet-4-6", etc.
- **MAC suffix**: Above WiFi icon — for configuring hooks
- **WiFi icon + SSID**: Bottom area

## Session Work Timer

The innermost orange ring tracks active working time:
- **Starts** when Claude begins working (state 2)
- **Pauses** when waiting for input (state 3) or idle (state 0)
- **Resumes** when Claude starts working again
- **Resets** on new session (transition from IDLE/FINISHED to WORKING)

## States

| State | Label | Display | Trigger |
|-------|-------|---------|---------|
| 0 | IDLE | Dim cyan, static | UserPromptSubmit / default |
| 2 | WORKING | Pulsing magenta | PreToolUse |
| 3 | NEED INPUT | Flashing pink/yellow | Notification |
| 5 | FINISHED | Green | Stop |
| 7 | ERROR | Flashing red | (manual or custom hook) |

## Hook Script

The `hooks/claude-claw-hook.sh` script:
- Reads hook JSON from stdin (Claude Code passes this automatically)
- Extracts model name and caches it across hook invocations
- Parses the session transcript JSONL for output token counts
- Computes approximate session usage % against the plan limit
- Publishes extended MQTT message: `state|model|session_pct|weekly_pct|reset_epoch|client_host`

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CLAW_TOPIC` | (required) | MQTT topic, e.g. `iotj/cl/openwr/updates/b7a4` |
| `CLAW_BROKER` | `broker.hivemq.com` | MQTT broker hostname |
| `CLAW_PLAN` | `80000` | Token limit per 5hr window |

Plan limit estimates: Pro=45000, Max5=80000, Max20=200000. Set via:
```
CLAW_PLAN=200000 CLAW_TOPIC='iotj/cl/openwr/updates/b7a4' ~/.claude/hooks/claude-claw-hook.sh
```

### Session Usage

Session % is estimated from output tokens in the transcript. Weekly % is not yet available from Claude Code (displays "?"). Multiple GitHub feature requests are open (#11008, #11535, #36056). When Anthropic ships this data, only the hook script needs updating.

## REPL Commands

Connect via USB-C (115200 baud) to access the Forth REPL.

### WiFi
| Command | Description |
|---------|-------------|
| `wifi-ssid` | Set WiFi SSID |
| `wifi-pass` | Set WiFi password (Enter for open network) |
| `wifi-connect` | Save credentials and connect |
| `wifi-status` | Show SSID, IP, hostname, connection state |
| `wifi-clear` | Erase stored WiFi credentials |

### MQTT
| Command | Description |
|---------|-------------|
| `mqtt-broker` | View/change MQTT broker URI |
| `mqtt-status` | Show broker, topic, connected state |
| `mqtt-topic` | Print subscription topic |

### Display
| Command | Description |
|---------|-------------|
| `N brightness` | Set display brightness 0-100 |
| `bright?` | Show current brightness |
| `invert` | Toggle display color inversion |

### Sound
| Command | Description |
|---------|-------------|
| `sound-on` | Enable chimes on state changes (saved to NVS) |
| `sound-off` | Disable chimes (saved to NVS) |

### Status
| Command | Description |
|---------|-------------|
| `status` | Print current state, model, session %, sound setting |

## Sound / Chimes

Default: **OFF**. Toggle with button press or `sound-on` / `sound-off`.

| State Change | Chime |
|--------------|-------|
| WORKING | Soft 800Hz, 50ms |
| FINISHED | Ascending 1200Hz + 1800Hz |
| NEED INPUT | 2000Hz, 100ms |
| ERROR | Descending 1000Hz + 600Hz |

## Hardware

- **Board**: M5Stack Dial (ESP32-S3, 8MB flash, no PSRAM)
- **Display**: GC9A01 240x240 round IPS LCD (SPI)
- **Touch**: FT5x06 capacitive (I2C) — unused, reserved for future
- **Encoder**: Rotary quadrature on GPIO 40/41 — adjusts brightness
- **Speaker**: Buzzer on GPIO 3 (LEDC PWM)
- **Button**: GPIO 42 — toggles sound on/off

## Testing Manually

```bash
# Send a working notification with model and session %
mosquitto_pub -h broker.hivemq.com \
  -t "iotj/cl/openwr/updates/b7a4" \
  -m "2|opus-4-6|42|-1|0|my-laptop"

# Send finished
mosquitto_pub -h broker.hivemq.com \
  -t "iotj/cl/openwr/updates/b7a4" -m "5"

# Monitor what the device receives
mosquitto_sub -h broker.hivemq.com \
  -t "iotj/cl/openwr/updates/b7a4"
```

## HTTP API (same-subnet fallback)

| Endpoint | Description |
|----------|-------------|
| `GET /notify?state=N` | Set state (also supports &model=X&session=N&weekly=N&host=X) |
| `GET /status` | JSON: state, model, IP, MAC, hostname, uptime |
| `GET /ping` | Returns "pong" |

## Project Structure

```
M5StackDial-Blinky_Claw_OpenWR/
  platformio.ini              # PlatformIO: espidf, S3, 2MB app partition
  CMakeLists.txt
  partitions.csv              # Custom partition table (2MB app)
  sdkconfig.defaults          # ESP-IDF config
  sdkconfig.esp32s3           # Disables PSRAM for M5Dial
  src/
    CMakeLists.txt
    idf_component.yml         # m5gfx + mdns dependencies
    main.cpp                  # Full application (~1500 lines)
  components/
    forth/                    # ESPIDFORTH v0.2.0
  hooks/
    claude-claw-hook.sh       # Hook script for Claude Code
  docs/
    M5StackDial-Blinky_Claw_OpenWR-design.md
  LICENSE                     # MIT, Copyright 2026 IoTone, Inc.
```

## Memory Usage

| Metric | Value |
|--------|-------|
| Flash | 1050 KB (50% of 2 MB partition) |
| RAM | 96 KB (29% of 328 KB) |
| Forth heap | 48 KB |

## Also See

- **M5Stamp3CU_Blinky_Claw_OpenWR** — LED-only version for M5Stamp C3U (same MQTT topic, simpler hardware)
- **ESPIDFORTH** — The Forth interpreter component used by this project
- **hooks/claude-claw-hook.sh** — The hook script (install to `~/.claude/hooks/`)
