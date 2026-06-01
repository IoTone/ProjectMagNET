# Claw OpenWR — Design Document

## Overview

Claw OpenWR is an ambient LED notifier for Claude Code CLI sessions. An M5Stamp C3U (ESP32-C3) device connects to WiFi and runs a tiny HTTP server. Claude Code hooks fire `curl` commands on lifecycle events (task complete, needs input, error), setting the LED pattern accordingly. The device is discoverable via mDNS as `ClawOpenWR-XXXX.local`.

Built on ESPIDFORTH (ESP-IDF + Forth REPL), the device is configurable at runtime via USB serial.

## Problem Statement

When Claude Code runs in a terminal — possibly in the background, on a remote machine, or in one of many tabs — there's no physical indicator of its state. You have to keep checking. Claw OpenWR provides ambient awareness: a single RGB LED tells you at a glance whether Claude needs your attention, finished a task, or hit an error.

## Hardware

- **Board**: M5Stamp C3U (ESP32-C3, RISC-V, 160MHz, 4MB flash, no PSRAM)
- **LED**: SK6812 NeoPixel on GPIO 2 (1 RGB LED)
- **Button**: GPIO 9 (active low, internal pull-up)
- **Console**: USB-serial-JTAG (REPL over USB-C)
- **WiFi**: 802.11 b/g/n (2.4 GHz)

## LED Modes

| Mode | Name | LED Pattern | Trigger |
|------|------|-------------|---------|
| 0 | Off | LED dark | Idle / default |
| 1 | Random Flash | Random colors every 200ms | Manual (button/REPL) |
| 2 | Slow Breathe | Fade random hue in/out, ~2s cycle | Manual (button/REPL) |
| 3 | Attention | Fast strobe random colors, 50ms on/off | **Claude needs input** |
| 4 | Rainbow | HSV hue rotation, ~2s full cycle | Manual (button/REPL) |
| 5 | Task Complete | Green fade-in pulse, 1000ms cycle | **Claude finished task** |
| 6 | Network Offline | Solid pink (255, 105, 180) | WiFi disconnected |
| 7 | Error | Red strobe (255, 0, 0), 100ms on/off | **Claude error** |

- **Button** cycles through modes 0-4 only (manual/decorative modes)
- **Modes 5-7** are notification modes, set via HTTP API or REPL

## Communication: HTTP + mDNS

### Why This Approach

| Approach | ESP32 Effort | Host Effort | Needs Infra? | Reliable? |
|----------|-------------|-------------|--------------|-----------|
| **HTTP + mDNS** | Low | Zero (`curl`) | No | Yes (HTTP ack) |
| UDP broadcast | Low | Low (`nc`) | No | No (no ack) |
| MQTT | Medium | Medium | Yes (broker) | Yes |
| WebSocket | Medium-High | Medium | No | Yes |

**Both HTTP and MQTT are implemented.** They run simultaneously:
- **HTTP + mDNS**: Best for same-subnet networks. Zero host-side setup (`curl`).
- **MQTT**: Best for cross-subnet/VLAN networks. Uses a public broker by default (`broker.hivemq.com:1883`), no infrastructure to run. Works from anywhere with internet access.

### HTTP API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/notify?state=N` | GET | Set LED to mode N (0-7). Returns `"ok"`. |
| `/status` | GET | JSON: `{"mode":N,"mode_name":"...","ip":"...","mac":"...","hostname":"...","uptime_s":N}` |
| `/ping` | GET | Returns `"pong"`. Health check. |

### Claude Code Hooks

Claude Code fires shell commands on lifecycle events via `settings.json`:

```json
{
  "hooks": {
    "Stop": [{
      "matcher": "",
      "hooks": [{
        "type": "command",
        "command": "curl -s -m 2 'http://ClawOpenWR-XXXX.local/notify?state=5' || true"
      }]
    }],
    "Notification": [{
      "matcher": "",
      "hooks": [{
        "type": "command",
        "command": "curl -s -m 2 'http://ClawOpenWR-XXXX.local/notify?state=3' || true"
      }]
    }]
  }
}
```

Replace `XXXX` with your device's last 4 MAC hex digits (shown at boot and via `wifi-status`).

| Hook Event | Fires When | LED Mode |
|------------|-----------|----------|
| `Stop` | Claude finishes a response | 5 (green pulse) |
| `Notification` | Claude needs input, permission, or idle 60s+ | 3 (attention strobe) |
| `PostToolUse` | After a tool call (can match on errors) | 7 (red strobe) |

## MQTT

MQTT runs alongside HTTP for cross-subnet notification delivery. The device subscribes to a topic on a public broker; Claude Code hooks publish to it.

### Default Configuration

| Setting | Default | Configurable |
|---------|---------|-------------|
| Broker URI | `mqtt://broker.hivemq.com:1883` | `mqtt-broker` REPL command |
| Topic | `iotj/cl/openwr/updates/<last4_mac>` | Auto-derived from MAC |
| QoS | 1 (at-least-once) | Hardcoded |
| Message format | Plain integer `0`-`7` | — |

### REPL Commands

| Command | Description |
|---------|-------------|
| `mqtt-broker` | View/change broker URI, saves to NVS, reconnects |
| `mqtt-status` | Print broker, topic, connected state |
| `mqtt-topic` | Print just the subscription topic (for configuring hooks) |

### Claude Code Hook (MQTT variant)

Install mosquitto tools: `brew install mosquitto`

```json
{
  "hooks": {
    "Stop": [{
      "matcher": "",
      "hooks": [{
        "type": "command",
        "command": "mosquitto_pub -h broker.hivemq.com -t 'iotj/cl/openwr/updates/XXXX' -m '5' 2>/dev/null || true"
      }]
    }],
    "Notification": [{
      "matcher": "",
      "hooks": [{
        "type": "command",
        "command": "mosquitto_pub -h broker.hivemq.com -t 'iotj/cl/openwr/updates/XXXX' -m '3' 2>/dev/null || true"
      }]
    }]
  }
}
```

Replace `XXXX` with your device's last 4 MAC hex digits (shown via `mqtt-topic`).

### Testing MQTT

```bash
# Check your topic
# (from REPL) mqtt-topic
# Output: iotj/cl/openwr/updates/aaa4

# Send a notification from any machine
mosquitto_pub -h broker.hivemq.com -t "iotj/cl/openwr/updates/aaa4" -m "3"

# Monitor what the device receives
mosquitto_sub -h broker.hivemq.com -t "iotj/cl/openwr/updates/aaa4"
```

### Security Note

Public brokers mean anyone who discovers the topic can send messages. The risk is minimal (worst case: someone toggles your LED color). For sensitive environments, use `mqtt-broker` to point to a private broker with authentication.

## WiFi

### Configuration via Forth REPL

| Command | Description |
|---------|-------------|
| `wifi-ssid` | Prompts interactively for SSID |
| `wifi-pass` | Prompts for password (press Enter for open networks) |
| `wifi-connect` | Saves credentials to NVS and initiates connection |
| `wifi-status` | Print SSID, connection state, IP, hostname |
| `wifi-clear` | Erase stored credentials from NVS flash |

### Security

- **Open networks**: Leave password empty (press Enter at `wifi-pass` prompt)
- **WPA2-PSK**: Standard password authentication
- **WPA3-SAE**: Supported by ESP-IDF WiFi driver (auto-negotiated)

### Credential Storage

Credentials are stored in NVS (Non-Volatile Storage) flash:
- Namespace: `claw_wifi`
- Keys: `ssid` (string, max 32 chars), `pass` (string, max 64 chars)
- Persist across reboots until explicitly cleared with `wifi-clear`

### Hostname

Format: `ClawOpenWR_XXXX` where `XXXX` is the last 4 hex digits of the WiFi MAC address.

mDNS advertises as `ClawOpenWR-XXXX.local` (hyphen for DNS compatibility) with an `_http._tcp` service on port 80.

### Connection Behavior

- **Boot with stored creds**: Auto-connect. LED is solid pink (mode 6) during connection attempt. On success: green strobe for 5 seconds, then mode 0 (off). On failure after 15s timeout: stay pink.
- **Boot without creds**: Mode 0 (off). Configure via REPL.
- **Disconnect during operation**: LED goes pink (mode 6). Auto-reconnect attempts with exponential backoff (1s, 2s, 4s, 8s, 16s), max 5 retries.
- **Successful reconnect**: Green strobe 5 seconds, restore previous mode.

## Boot Sequence

```
1. Init USB-serial-JTAG driver
2. Print banner (version, build date)
3. Init LED (GPIO 2), button (GPIO 9), NVS flash
4. Derive hostname from MAC address
5. Init WiFi subsystem (STA mode, set hostname)
6. Init ESPIDFORTH engine (48 KB heap)
7. Register Forth words (blinky, wifi-*, modes, led-rgb, etc.)
8. Start LED pattern task (FreeRTOS, 4 KB stack)
9. Check NVS for stored WiFi credentials
   ├─ Found: attempt WiFi connect (mode 6 while connecting)
   │  ├─ Success: start HTTP server + mDNS, green strobe 5s, mode 0
   │  └─ Failure: stay mode 6, log error
   └─ Not found: mode 0 (off), print REPL instructions
10. Run Forth REPL in app_main (blocks forever)
```

## Architecture

```
┌──────────────────┐     ┌──────────────────┐
│   LED Task       │     │   app_main       │
│   (patterns)     │     │   (Forth REPL)   │
│                  │     │                  │
│ pattern_off      │◄───►│ blinky           │
│ pattern_flash    │     │ led-rgb          │
│ pattern_breathe  │     │ wifi-ssid        │
│ pattern_strobe   │     │ wifi-pass        │
│ pattern_rainbow  │     │ wifi-connect     │
│ pattern_green    │     │ wifi-status      │
│ pattern_pink     │     │ wifi-clear       │
│ pattern_red      │     │                  │
│ check_button     │     │ forth_repl()     │
└────────┬─────────┘     └────────┬─────────┘
         │                        │
    ┌────┴────┐              USB-serial-JTAG
    │ led_strip│
    │ (RMT)   │
    └─────────┘
         │
┌────────┴─────────┐
│   WiFi Event     │     ┌──────────────────┐
│   Handler        │     │  HTTP Server     │
│                  │     │  (httpd task)    │
│ STA_START        │     │                  │
│ STA_DISCONNECTED │     │ /notify?state=N  │
│ GOT_IP           │────►│ /status          │
│                  │     │ /ping            │
└──────────────────┘     └──────────────────┘
                                │
                          ┌─────┴─────┐
                          │   mDNS    │
                          │ .local    │
                          └───────────┘
```

## Battery / Headless Operation

The device is designed to run unattended on battery power with no USB connection. Once WiFi is configured (credentials stored in NVS), the full boot-to-HTTP-ready sequence is automatic:

1. Power on → auto-connect WiFi from stored creds → start HTTP server + mDNS
2. LED shows pink while connecting, green strobe on success, then mode 0 (off)
3. HTTP endpoints accept notifications from Claude Code hooks
4. Button still works for manual mode cycling

**No UART/USB connection required after initial WiFi configuration.**

The Forth REPL task will idle harmlessly when no USB host is connected — `usb_serial_jtag_read_bytes` returns immediately with no data, the task yields, and other tasks continue unaffected. The USB-serial-JTAG driver is still installed (for reconnecting to REPL later) but consumes minimal resources (~2 KB).

To reconfigure WiFi on battery (no USB): press and hold the button for 5 seconds to trigger a WiFi credential clear + reboot. This forces the device back to mode 0 so you can reconnect USB and reconfigure. (Alternatively, use the HTTP `/notify?state=0` endpoint from any device on the network.)

## Forth REPL Commands (Complete)

### Blinky / LED Control
| Command | Stack | Description |
|---------|-------|-------------|
| `N blinky` | ( n -- ) | Set mode 0-7 |
| `modes` | ( -- ) | List all modes |
| `mode?` | ( -- n ) | Push current mode |
| `R G B led-rgb` | ( r g b -- ) | Set LED color directly (stops pattern) |
| `led-off` | ( -- ) | Turn off LED |

### WiFi
| Command | Stack | Description |
|---------|-------|-------------|
| `wifi-ssid` | ( -- ) | Prompt for SSID string |
| `wifi-pass` | ( -- ) | Prompt for password string |
| `wifi-connect` | ( -- ) | Save to NVS and connect |
| `wifi-status` | ( -- ) | Print connection info |
| `wifi-clear` | ( -- ) | Erase stored credentials |

### MQTT
| Command | Stack | Description |
|---------|-------|-------------|
| `mqtt-broker` | ( -- ) | View/change broker URI (saves to NVS, reconnects) |
| `mqtt-status` | ( -- ) | Print broker, topic, connected state |
| `mqtt-topic` | ( -- ) | Print subscription topic |

### Standard ESPIDFORTH
All standard words (arithmetic, stack, comparison, logic, control flow, memory, etc.) plus `mem`, `free-heap`, `chip-info`, `words`.

## Memory Budget (ESP32-C3, 328 KB SRAM)

| Layer | Size | Notes |
|-------|------|-------|
| Flash (firmware) | 935 KB (89%) | WiFi + HTTP + MQTT + mDNS + Forth |
| Static RAM | 89 KB (27%) | ESP-IDF + WiFi + Forth globals |
| Forth heap | 48 KB | Reduced from 64 KB for WiFi headroom |
| WiFi buffers | ~25 KB | Dynamic, allocated by WiFi driver |
| HTTP server | ~4 KB | Task stack + connection buffers |
| MQTT client | ~10 KB | Task stack + connection buffers |
| mDNS | ~2 KB | Service records |
| Free heap | ~80 KB | Available for Forth scripts |

## Project Structure

```
M5Stamp3CU_Blinky_Claw_OpenWR/
  .gitignore
  platformio.ini                # PlatformIO: espidf, esp32-c3-devkitc-02
  CMakeLists.txt                # Top-level CMake
  sdkconfig.defaults            # ESP-IDF config (WiFi enabled, BT disabled)
  src/
    CMakeLists.txt              # Component: forth, esp_wifi, esp_http_server, mdns, nvs_flash, led_strip
    idf_component.yml           # led_strip managed component
    main.c                      # All application code (~800 lines)
  components/
    forth/                      # ESPIDFORTH v0.2.0
  docs/
    Claw_OpenWR-design.md       # This document
```

## Testing Plan

| # | Test | Expected Result |
|---|------|----------------|
| 1 | Boot with no stored WiFi | Mode 0 (off), REPL accessible, prints instructions |
| 2 | `wifi-ssid` then `wifi-pass` then `wifi-connect` | Green strobe 5s, then mode 0 |
| 3 | `wifi-status` after connect | Shows SSID, IP, hostname |
| 4 | `curl http://ClawOpenWR-XXXX.local/ping` | Returns "pong" |
| 5 | `curl 'http://host.local/notify?state=3'` | LED: fast strobe (attention) |
| 6 | `curl 'http://host.local/notify?state=5'` | LED: green pulse (complete) |
| 7 | `curl 'http://host.local/notify?state=7'` | LED: red strobe (error) |
| 8 | `curl 'http://host.local/notify?state=0'` | LED off |
| 9 | `curl http://host.local/status` | JSON with mode, IP, MAC, hostname |
| 10 | Reboot device | Auto-reconnects, green strobe, mode 0 |
| 11 | `wifi-clear` then reboot | No auto-connect, mode 0 |
| 12 | Disconnect WiFi (router off) | LED goes pink (mode 6) |
| 13 | Button press | Cycles through modes 0-4 only |
| 14 | Configure Claude Code hooks, run a task | LED changes on Stop/Notification events |
