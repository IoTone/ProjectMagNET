# M5StackDial-Blinky_Claw_OpenWR — Design Plan

## Context

Build a synthwave-themed Claude Code status display on the M5Stack Dial. Merges the M5Dial demo (display, encoder, speaker, Forth REPL) with the Claw OpenWR (WiFi, MQTT, HTTP, NVS). Shows a fiddler crab mascot, Claude CLI state, model name, session/weekly usage rings, and WiFi status — all driven by MQTT messages from Claude Code hooks.

## Display Layout (240x240 round LCD)

```
        ┌──── "SESSION" label ────┐
       ╱  Outer ring: session %    ╲
      │  ╱ Inner ring: weekly %  ╲  │
      │ │ ╱ Timer ring (orange) ╲ │ │
      │ │ │    00:12:34          │ │ │
      │ │ │  ── WORKING ──       │ │ │
      │ │ │   (macbook-pro)      │ │ │
      │ │ │                      │ │ │
      │ │ │  🦀 Fiddler Crab     │ │ │
      │ │ │                      │ │ │
      │ │ │    "opus-4-6"        │ │ │
      │ │  ╲                    ╱ │ │
      │  ╲  "WEEKLY" label      ╱  │
       ╲      b7a4              ╱
        ╲    📶 MyNetwork      ╱
         └───────────────────────┘
```

- **Background**: Deep purple-black with synthwave perspective grid lines
- **Outer ring**: Session usage % (100 ticks, cyan→yellow→red gradient)
- **Inner ring**: Weekly usage % (50 ticks, thinner, same gradient)
- **Timer ring**: Session work time (60 ticks = 60 minutes, orange, wraps)
- **Timer text**: `hh:mm:ss` above status text, pulses orange when running, dims when paused
- **Center**: Fiddler crab drawn with neon primitives (cyan body, magenta claws, green eyes)
- **Status text**: Above crab — IDLE / WORKING / FINISHED / NEED INPUT / ERROR
- **Client hostname**: Below status in parens — "(macbook-pro)" in dim text
- **Model name**: Below crab — "opus-4-6", "sonnet-4-6", etc.
- **MAC suffix**: Above WiFi icon — "b7a4" (for configuring hooks)
- **WiFi icon + SSID**: Bottom area (persists during session)

## States

| State | Label | Text Color | Animation |
|-------|-------|------------|-----------|
| 0 | IDLE | Dim cyan | Static |
| 2 | WORKING | Magenta | Pulsing brightness |
| 3 | NEED INPUT | Hot pink/yellow | Alternating 500ms |
| 5 | FINISHED | Neon green | Static (flash on transition) |
| 7 | ERROR | Red | Flashing 300ms |

## MQTT Message Format (Extended, Backward-Compatible)

Topic: `iotj/cl/openwr/updates/<last4_mac>` (same as Claw OpenWR)

Format: `state|model|session_pct|weekly_pct|reset_epoch|client_host`

Examples:
- `2|opus-4|42|18|1743580800|macbook-pro` — working, 42% session, from macbook-pro
- `5` — plain integer, backward compatible (state only)
- `3|sonnet-4|-1|-1|0|dev-server` — need input, usage unknown, from dev-server

The `client_host` field is the short hostname of the machine running Claude Code (captured via `$(hostname -s)` in the hook).

Parser: if `|` found → sscanf extended format. Otherwise → atoi state only.

**Plan limits are aspirational** — Claude Code doesn't expose session/weekly % programmatically today (multiple open feature requests). The hook script sends `-1` for unknown values. The display shows "?" when values are -1. When Anthropic ships the data, only the hook script needs updating.

## Synthwave Color Palette

```
BG         = rgb(12, 4, 20)      // Deep purple-black
CYAN       = rgb(0, 255, 255)
MAGENTA    = rgb(255, 0, 255)
HOT_PINK   = rgb(255, 105, 180)
NEON_GREEN = rgb(57, 255, 20)
YELLOW     = rgb(255, 255, 0)
RED        = rgb(255, 40, 40)
DIM_CYAN   = rgb(0, 60, 60)
GRID       = rgb(40, 20, 60)     // Perspective grid lines
TEXT_DIM   = rgb(100, 80, 120)
```

## Fiddler Crab (Geometric Primitives)

~80x60 pixel area centered on display. Built from:
- Body: overlapping filled circles (cyan)
- Big claw (right): large circles (magenta/hot pink) — asymmetric!
- Small claw (left): smaller circles
- Legs: 3 angled lines per side (cyan)
- Eye stalks: lines + small filled circles (neon green)
- Shell accents: lighter cyan lines

~50-70 lines of `fillCircle` / `drawLine` calls. Will need pixel-tuning on hardware.

## Session Work Timer

The 3rd innermost ring (orange) tracks active working time during a Claude Code session.

- **60 ticks** = 60 minutes (1 per minute, wraps after 1 hour)
- **Starts** when state changes to WORKING (2) — resets if coming from IDLE or FINISHED
- **Pauses** when state changes to NEED INPUT (3) or IDLE (0)
- **Resumes** when state returns to WORKING (2)
- **Stops** on FINISHED (5) — time remains displayed
- **hh:mm:ss** shown above the status text
- Text pulses orange when running, dims when paused
- Ticks: orange (0-44 min), deep orange (45-60 min)

## Sound System

- **Default: OFF** — persisted in NVS key `"sound"`
- **Toggle**: button press or `sound-on` / `sound-off` REPL commands
- **Chimes on state change**:
  - WORKING: soft 800Hz, 50ms
  - FINISHED: ascending 1200Hz→1800Hz
  - NEED INPUT: 2000Hz, 100ms
  - ERROR: descending 1000Hz→600Hz

## Inputs

- **Encoder rotation**: Display brightness (reuse existing pattern)
- **Button press**: Toggle sound on/off
- **Touch**: Unused (placeholder for future)

## WiFi / MQTT / HTTP / mDNS

Copied from Claw OpenWR with these changes:
- Hostname prefix: `FiddlerCrab` (mDNS: `fiddlercrab-XXXX.local`)
- WiFi disconnect: update display WiFi icon (not LED mode)
- WiFi connect: brief green flash on WiFi icon, start HTTP + mDNS + MQTT
- SNTP time sync on connect (for reset countdown display)
- MQTT parser extended for pipe-delimited format

Same REPL commands: `wifi-ssid`, `wifi-pass`, `wifi-connect`, `wifi-status`, `wifi-clear`, `mqtt-broker`, `mqtt-status`, `mqtt-topic`

## Forth REPL Commands

| Command | Description |
|---------|-------------|
| `wifi-ssid` | Set WiFi SSID |
| `wifi-pass` | Set WiFi password |
| `wifi-connect` | Save and connect |
| `wifi-status` | Show connection info |
| `wifi-clear` | Erase credentials |
| `mqtt-broker` | View/change MQTT broker |
| `mqtt-status` | Show MQTT state |
| `mqtt-topic` | Print subscription topic |
| `N brightness` | Set display brightness 0-100 |
| `bright?` | Push current brightness |
| `sound-on` | Enable chimes (saved to NVS) |
| `sound-off` | Disable chimes (saved to NVS) |
| `status` | Print current state/model/usage |

## Architecture

```
┌─────────────────────┐     ┌──────────────────┐
│   Main Loop         │     │  Forth REPL Task │
│   (display render)  │     │  (background)    │
│                     │     │                  │
│  draw_synthwave_bg  │◄───►│  wifi-*          │
│  draw_crab          │     │  mqtt-*          │
│  draw_rings         │     │  sound-on/off    │
│  draw_status_text   │     │  brightness      │
│  draw_wifi_icon     │     │  status          │
│  handle_encoder     │     │                  │
│  handle_button      │     │  forth_repl()    │
│  speaker_update     │     │                  │
└────────┬────────────┘     └────────┬─────────┘
         │                           │
    ┌────┴────┐                 USB-serial-JTAG
    │  M5GFX  │
    │  PCNT   │
    │  LEDC   │
    └─────────┘
         │
┌────────┴─────────┐
│  WiFi + MQTT     │     ┌──────────────────┐
│  Event Handlers  │     │  HTTP Server     │
│                  │     │  /notify /status  │
│  set dirty flags │────►│  /ping           │
│  update state    │     │                  │
└──────────────────┘     └──────────────────┘
```

Display rendering in main loop (not thread-safe M5GFX). MQTT/HTTP callbacks only set dirty flags + state variables. Main loop checks flags and redraws.

## Render Strategy (Anti-Flicker)

- Background + crab: draw once on init
- Status text: clear with `setTextColor(fg, BG)` (M5GFX draws opaque background behind text)
- Rings: only redraw when usage data changes (throttled to max every 5 min)
- WiFi icon: only redraw on connection state change
- Animations (WORKING pulse, NEED INPUT flash): redraw status text area only, ~every 500ms

## Boot Sequence

1. Init USB serial, display, encoder, speaker, button, NVS
2. Draw synthwave background + crab + placeholder text
3. Derive hostname, init WiFi subsystem
4. Load sound preference, MQTT broker from NVS
5. Init Forth engine (48 KB heap) + register words
6. Start Forth REPL task (background, priority 3)
7. Auto-connect WiFi from NVS (if credentials stored)
8. Main render loop (handles encoder, button, speaker, dirty flag redraws)

## Project Structure

```
M5StackDial-Blinky_Claw_OpenWR/
  .gitignore
  platformio.ini
  CMakeLists.txt
  sdkconfig.defaults
  sdkconfig.defaults.esp32s3
  sdkconfig.esp32s3              # PIO trick: disables PSRAM
  src/
    CMakeLists.txt
    idf_component.yml            # m5gfx + mdns
    main.cpp                     # ~1200-1400 lines
  components/
    forth/                       # ESPIDFORTH v0.2.0
  hooks/
    claude-claw-hook.sh          # Hook script for extended MQTT format
  docs/
    M5StackDial-Blinky_Claw_OpenWR-design.md
```

## Memory Budget (ESP32-S3, 512 KB SRAM, 8 MB flash, no PSRAM)

| Layer | Estimated | Notes |
|-------|-----------|-------|
| Flash | ~500-550 KB | M5GFX + WiFi + MQTT + HTTP + mDNS + Forth |
| Static RAM | ~100 KB | Display + WiFi + MQTT buffers |
| Forth heap | 48 KB | Reduced for WiFi headroom |
| Free heap | ~70-80 KB | Comfortable on S3 |

## Claude Code Hook Config

```json
{
  "hooks": {
    "PreToolUse": [{
      "matcher": "",
      "hooks": [{
        "type": "command",
        "command": "mosquitto_pub -h broker.hivemq.com -t 'iotj/cl/openwr/updates/XXXX' -m \"2|unknown|-1|-1|0|$(hostname -s)\" 2>/dev/null || true"
      }]
    }],
    "Stop": [{
      "matcher": "",
      "hooks": [{
        "type": "command",
        "command": "mosquitto_pub -h broker.hivemq.com -t 'iotj/cl/openwr/updates/XXXX' -m \"5|unknown|-1|-1|0|$(hostname -s)\" 2>/dev/null || true"
      }]
    }],
    "Notification": [{
      "matcher": "",
      "hooks": [{
        "type": "command",
        "command": "mosquitto_pub -h broker.hivemq.com -t 'iotj/cl/openwr/updates/XXXX' -m \"3|unknown|-1|-1|0|$(hostname -s)\" 2>/dev/null || true"
      }]
    }]
  }
}
```

### Recommended: Hook Script (includes model name + session usage)

The `hooks/claude-claw-hook.sh` script reads the hook JSON from stdin, extracts the model name, parses the session transcript JSONL for token usage, and publishes the extended MQTT format. Requires `jq` and `mosquitto_pub` (`brew install jq mosquitto`).

```json
{
  "hooks": {
    "PreToolUse": [{"matcher": "", "hooks": [{"type": "command",
      "command": "CLAW_TOPIC='iotj/cl/openwr/updates/XXXX' /path/to/hooks/claude-claw-hook.sh"}]}],
    "Stop": [{"matcher": "", "hooks": [{"type": "command",
      "command": "CLAW_TOPIC='iotj/cl/openwr/updates/XXXX' /path/to/hooks/claude-claw-hook.sh"}]}],
    "Notification": [{"matcher": "", "hooks": [{"type": "command",
      "command": "CLAW_TOPIC='iotj/cl/openwr/updates/XXXX' /path/to/hooks/claude-claw-hook.sh"}]}],
    "UserPromptSubmit": [{"matcher": "", "hooks": [{"type": "command",
      "command": "CLAW_TOPIC='iotj/cl/openwr/updates/XXXX' /path/to/hooks/claude-claw-hook.sh"}]}]
  }
}
```

Environment variables:
- `CLAW_TOPIC` (required): your device's MQTT topic
- `CLAW_BROKER` (default: `broker.hivemq.com`): MQTT broker
- `CLAW_PLAN` (default: `80000`): token limit per 5hr window (Pro=45000, Max5=80000, Max20=200000)

The script sends: `state|model|session_pct|weekly_pct|reset_epoch|client_host`

**Session usage %** is estimated from output tokens in the transcript JSONL. Weekly usage is not yet available from Claude Code (-1).

### Note on Plan Limits

Claude Code does not expose session/weekly usage % programmatically (open feature requests: #11008, #11535, #36056). The hook script estimates session % from transcript token counts against a hardcoded plan limit. Weekly % shows "?" until Anthropic ships this data. When they do, only the hook script needs updating — the device firmware is already ready.

## Implementation Order

1. **Scaffold + display** — config files, LGFX class, synthwave background, crab drawing. Verify renders.
2. **Rings** — session + weekly rings with hardcoded test percentages.
3. **WiFi + MQTT + HTTP** — port from Claw OpenWR, extend MQTT parser. Verify connectivity.
4. **Forth REPL + sound** — all REPL words, sound toggle, NVS persistence.
5. **Status animations** — pulsing/flashing text, WiFi icon, chimes on transition.
6. **Hook script + docs** — claude-claw-hook.sh, design doc, README.

## Verification

1. Build + flash to M5Dial — display shows synthwave crab
2. Encoder rotation changes brightness
3. Button press toggles sound
4. `wifi-ssid` / `wifi-pass` / `wifi-connect` — WiFi icon updates
5. `mosquitto_pub -h broker.hivemq.com -t "iotj/cl/openwr/updates/XXXX" -m "2|opus-4|42|18|0"` — display shows WORKING, opus-4, rings at 42%/18%
6. Test all states: 0, 2, 3, 5, 7
7. Backward compat: `mosquitto_pub ... -m "3"` — plain integer still works
8. Reboot — auto-reconnects, restores sound preference
9. Claude Code hooks end-to-end

## Things We Might Have Missed

1. **Screen burn-in**: The round LCD is IPS so no OLED burn-in risk, but consider subtle idle animation (crab eyes blink every ~10s?) to show the device is alive
2. **Multiple Claude sessions**: Last-write-wins on the MQTT topic. Acceptable for single-user.
3. **NTP failure**: If SNTP doesn't sync, omit reset time display. Show "?" instead.
4. **PSRAM gotcha**: M5Dial has no PSRAM. Must use `sdkconfig.esp32s3` with `# CONFIG_SPIRAM is not set` (PIO workaround we discovered).
5. **Topic sharing**: Both the C3U LED device and this Dial display can subscribe to the same topic simultaneously — one LED, one visual display, from the same Claude session.
6. **Future**: encoder rotation and touch are reserved for future features (scroll through history? dismiss notifications? change display mode?)
