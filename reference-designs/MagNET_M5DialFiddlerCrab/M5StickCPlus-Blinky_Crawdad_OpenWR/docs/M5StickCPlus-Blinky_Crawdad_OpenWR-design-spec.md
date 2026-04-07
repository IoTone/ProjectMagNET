# M5StickCPlus-Blinky_Crawdad_OpenWR — Design Spec

## Overview

A multi-session Claude Code monitoring dashboard on the M5StickC Plus. Monitors **multiple concurrent Claude CLI sessions** on a 240×135 landscape display, showing each session's state, timer, token usage, and model in color-coded rows with animated crawdad characters.

Shares WiFi, NVS/profiles, MQTT, HTTP, speaker, and serial modules with the M5Dial project via shared ESP-IDF components in `../components/`. Uses ESPIDFORTH for REPL configuration.

## Hardware: M5StickC Plus (Original)

| Spec | Value |
|------|-------|
| SoC | ESP32-PICO-D4 (classic ESP32, dual-core 240MHz) |
| Flash | 4 MB |
| PSRAM | None |
| Display | 135×240 ST7789V2 (SPI), used in **landscape** (240×135) via `setRotation(1)` |
| Backlight | AXP192 I2C (addr 0x34, SDA=21, SCL=22) — handled by M5GFX auto-detect |
| Button A | GPIO 37 (front, large) — active low |
| Button B | GPIO 39 (side, small) — active low |
| Speaker | GPIO 2 (PWM buzzer) |
| LED | GPIO 10 (active low) |
| IMU | MPU6886 (I2C, SDA=21, SCL=22) |
| Battery | 120 mAh built-in |
| USB | Type-C (CH552 USB-to-UART bridge) |

**Key differences from M5Dial:**
- Classic ESP32 (not S3) — UART serial via `driver/uart.h`, not USB-serial-JTAG
- Rectangular landscape display (240×135) — not round (240×240)
- No touch screen — two hardware buttons instead
- No rotary encoder — brightness cycled via settings menu
- AXP192 power management IC
- 4MB flash — custom 2MB app partition

## MQTT Multi-Session Design

### Topic Structure

```
iotj/cl/openwr/updates/<device_mac4>/<session_id>
```

The `session_id` is a full UUID (36 chars, e.g. `308c0f30-4ec0-4f9d-9145-08d5fe87ae1d`) provided by Claude Code in the hook JSON. The device subscribes to `iotj/cl/openwr/updates/<mac4>/#` (wildcard) and auto-discovers sessions as messages arrive.

**Backward compatibility**: The M5Dial Fiddler Crab subscribes to the flat topic `iotj/cl/openwr/updates/XXXX` and also receives per-session messages (MQTT delivers to both exact and wildcard matchers). Both devices can share the same `CRAW_TOPIC` base.

### Message Format (pipe-delimited)

```
state|model|session_pct|weekly_pct|reset_epoch|client_host
```

The `session_id` is extracted from the MQTT topic's last path segment by the `craw_mqtt` module — not from the message body.

### Session Auto-Discovery and Lifecycle

- **New message with unknown session_id** → create session, assign a neon color from round-robin palette
- **Max 8 sessions** — if full, evict the oldest session
- **Stale after 5 minutes** with no MQTT update → row dims to grey
- **Removed after 30 minutes** with no update → row disappears
- **Manual clear** via Settings screen → "Clear Sessions" item

### Session Data Structure

```c
#define MAX_SESSIONS         8
#define SESSION_ID_LEN      39  // UUIDs are 36 chars

typedef struct {
    char     id[SESSION_ID_LEN + 1];
    char     model[16];
    char     host[16];
    int      state;              // 0=IDLE, 2=WORKING, 3=NEED_INPUT, 5=FINISHED, 7=ERROR
    int      session_pct;        // -1 or 0-100
    uint32_t last_seen;          // millis() timestamp
    uint32_t work_start;
    uint32_t work_accumulated;
    bool     timer_running;
    bool     active;
    uint8_t  color_index;        // round-robin from palette
} session_t;
```

## Display Layout (240×135 landscape)

Uses M5GFX `M5Canvas` sprite for the sessions screen (double-buffered, flicker-free). WiFi and Settings screens draw directly to display (no animation, no flicker concern).

### Screen 1: WiFi Status (default on boot)

```
┌──────────────────────────────────────┐
│  CRAWDAD OpenWR              🦞      │  size 2, cyan, animated crawdad top-right
│──────────────────────────────────────│
│  WiFi: CIC-2.4GHz                   │  size 2, white
│  IP: 192.168.1.50                   │  size 2
│  MQTT: Connected  Sessions: 3       │  size 2, green/red for MQTT
│  Heap: 82340  Profile: default      │  size 2
│                                      │
│  [A]=Sess  [B]=Settings             │  size 1, dim
└──────────────────────────────────────┘
```

### Screen 2: Sessions Dashboard

```
┌──────────────────────────────────────┐
│  SESSIONS      🦞             3/8    │  size 2, crawdad green when any working
│──────────────────────────────────────│
│ ██ abc1 opus 02:15  8%          🦞  │  WORKING: dark yellow bg, white text, crawdad scurries
│ ██ def2 sonn 01:30  5%          🦞  │  FINISHED: dark green bg, white text, static crawdad
│ ██ ghi3 opus 00:00 12%          🦞  │  IDLE: grey bg, white text, static crawdad
│ ▓▓ jkl4 haik 05:20 30%          🦞  │  NEED INPUT: flashing blue bg (400ms cycle)
│ ▓▓ mno5 opus 00:10  2%          🦞  │  ERROR: flashing red bg (300ms cycle)
│                                  ▼   │  scroll indicator if more
│  [A]=Scroll  [B]=Back               │  size 1, dim
└──────────────────────────────────────┘
```

**Session row format** (size 2, ~20px tall):
- **Background bar** colored by state (dark tint — not full brightness, so white text is readable)
- **Session ID** (first 4 chars of UUID) — white text
- **Model** (4 chars: "opus"/"sonn"/"haik") — white text
- **Timer** (mm:ss of accumulated working time) — white text
- **Token %** (session usage) — white text
- **Crawdad** on far right — colored by session's assigned neon color, animated (legs scurry) when WORKING, static otherwise

**State color-coding** (background bar):

| State | Background | Behavior |
|-------|-----------|----------|
| IDLE | Dark grey | Static |
| WORKING | Dark yellow `rgb(60,50,0)` | Static bg, crawdad animates |
| NEED INPUT | Dark blue, flashing | Alternates `rgb(0,30,60)` / `rgb(0,15,30)` every 400ms |
| FINISHED | Dark green `rgb(10,50,5)` | Static |
| ERROR | Dark red, flashing | Alternates `rgb(60,10,10)` / `rgb(30,5,5)` every 300ms |
| Stale | `Synth::DIM_GRAY` | No crawdad |

5 session rows visible at once (scrollable with Button A).

### Screen 3: Settings

```
┌──────────────────────────────────────┐
│  SETTINGS                            │  size 2, cyan
│──────────────────────────────────────│
│  > Sound: OFF                        │  size 2, selected = green cursor
│    Bright: 80%                       │  size 2
│    Clear Sess: [B]                   │  size 2
│    Back: <-                          │  size 2
│                                      │
│  Hold[B] 3s=WiFi clr                │  size 1, red
│  [A]=Next  [B]=Toggle               │  size 1, dim
└──────────────────────────────────────┘
```

4 items: Sound, Brightness, Clear Sessions, **Back** (returns to WiFi screen).

## Button Navigation

| Button | WiFi Screen | Sessions Screen | Settings Screen |
|--------|-------------|-----------------|-----------------|
| **A** (front) | → Sessions | Scroll down (wrap) | Cycle items |
| **B** (side) | → Settings | → WiFi (back) | Toggle/activate |
| **B** (hold 3s) | Clear WiFi + reboot | Clear WiFi + reboot | Clear WiFi + reboot |

## Crawdad Character (Mini, ~20×12px)

Built from geometric primitives: 2 body circles, 2 eye dots, 2 claw lines with tips, 4 leg lines.

- **Animated**: legs shift ±1px every 150ms (scurry) when session is WORKING
- **Static**: legs in default position for all other states
- **Header crawdad**: neon green + animated when any session is WORKING; dim cyan + static otherwise
- **Per-row crawdad**: colored in session's assigned neon color (cyan/magenta/hot pink/neon green/yellow round-robin)
- Only drawn for non-stale sessions

Sessions screen uses `M5Canvas` sprite — all drawing (including crawdads) happens off-screen, then pushed to display in one atomic `pushSprite(0,0)` call. No flicker.

## Sound

Uses shared `craw_speaker` module. Speaker on GPIO 2 (PWM). Chimes on **all** state changes:

| State Change | Chime |
|-------------|-------|
| → WORKING | Soft 800Hz, 50ms |
| → FINISHED | Ascending 1200Hz → 1800Hz |
| → NEED INPUT | 2000Hz, 100ms |
| → ERROR | Descending 1000Hz → 600Hz |

Gated on `sound_enabled` (toggled via Settings screen or `sound-on`/`sound-off` REPL commands).

## Shared Components

```
MagNET_M5DialFiddlerCrab/
  components/                        # Shared ESP-IDF components
    craw_wifi/                       # WiFi STA + event callbacks
    craw_nvs/                        # NVS profiles + settings storage
    craw_mqtt/                       # MQTT client + message parser + session_id extraction
    craw_http/                       # HTTP server (/notify, /status, /ping)
    craw_mdns/                       # mDNS (OPTIONAL — gated by CRAW_ENABLE_MDNS, not compiled by default)
    craw_speaker/                    # PWM tone generation + chimes (GPIO parameterized)
    craw_serial/                     # Serial I/O abstraction (UART on ESP32, USB-JTAG on S3)
  M5StackDial-Blinky_Claw_OpenWR/   # Single-session, M5Dial (round 240×240)
    components/
      craw_* → ../../components/*    # Symlinks
      forth/                         # ESPIDFORTH
  M5StickCPlus-Blinky_Crawdad_OpenWR/  # Multi-session, M5StickC Plus (landscape 240×135)
    components/
      craw_* → ../../components/*    # Symlinks
      forth/                         # ESPIDFORTH (copy)
```

Components are symlinked into each project's `components/` directory. ESP-IDF discovers them automatically. No `EXTRA_COMPONENT_DIRS` or `lib_extra_dirs` needed.

### Module APIs (as implemented)

**craw_wifi.h** — WiFi STA with event callback:
```c
typedef enum { CRAW_WIFI_EVENT_CONNECTED, CRAW_WIFI_EVENT_DISCONNECTED, CRAW_WIFI_EVENT_CONNECT_FAILED } craw_wifi_event_t;
typedef void (*craw_wifi_event_cb_t)(craw_wifi_event_t event, void *ctx);
void craw_wifi_init(const char *hostname, craw_wifi_event_cb_t cb, void *cb_ctx);
void craw_wifi_connect(const char *ssid, const char *pass);
bool craw_wifi_is_connected(void);
bool craw_wifi_get_ip_str(char *buf, int len);
```

**craw_mqtt.h** — MQTT with parsed messages + session_id:
```c
typedef struct {
    int state; char model[32]; int session_pct; int weekly_pct;
    uint32_t reset_epoch; char client_host[32]; char session_id[40];
} craw_mqtt_msg_t;
typedef void (*craw_mqtt_msg_cb_t)(const craw_mqtt_msg_t *msg, void *ctx);
void craw_mqtt_init(const char *broker_uri, const char *base_topic, craw_mqtt_msg_cb_t msg_cb, craw_mqtt_conn_cb_t conn_cb, void *ctx);
void craw_mqtt_start(void);
bool craw_mqtt_is_connected(void);
```

**craw_speaker.h** — GPIO-parameterized speaker:
```c
void craw_speaker_init(int gpio_pin);  // M5Dial=3, M5StickC=2
void craw_speaker_tone(uint16_t freq, uint16_t duration_ms);
void craw_speaker_update(void);
void craw_speaker_set_sound_enabled(bool enabled);
void craw_speaker_chime_working(void);
void craw_speaker_chime_finished(void);
void craw_speaker_chime_need_input(void);
void craw_speaker_chime_error(void);
```

**craw_serial.h** — Auto-detects UART vs USB-JTAG at compile time:
```c
void craw_serial_init(void);
int  craw_serial_getchar(void);
void craw_serial_putchar(int c);
void craw_serial_print(const char *s);
void craw_serial_printf(const char *fmt, ...);
void craw_serial_read_line(char *buf, int maxlen, bool echo, bool allow_empty);
```

## Hook Script: claude-crawdad-hook.sh

Extends the claw hook with per-session MQTT topics. Caches model name and session token % per session_id in `~/.claude/.craw_cache/<session_id>/`.

```bash
SESSION_ID="$(echo "$INPUT" | jq -r '.session_id // "unknown"')"
mosquitto_pub -h "$CRAW_BROKER" -q 1 \
  -t "${CRAW_TOPIC}/${SESSION_ID}" \
  -m "${STATE}|${SHORT_MODEL}|${SESSION_PCT}|${WEEKLY_PCT}|${RESET_EPOCH}|${CLIENT_HOST}"
```

Environment variables:
- `CRAW_BROKER` — MQTT broker (default: `broker.hivemq.com`)
- `CRAW_TOPIC` — Base topic (e.g. `iotj/cl/openwr/updates/3038`)
- `CRAW_PLAN` — Token limit per 5hr window (default: 80000)

## Build Results (actual)

| Metric | Value |
|--------|-------|
| Flash | 994 KB (47.4% of 2MB partition) |
| RAM | 94 KB (28.7% of 328 KB) |
| Sprite buffer | ~64 KB heap (240×135×16bit for M5Canvas) |
| Platform | espressif32@6.9.0, ESP-IDF 5.3.1 |
| Board | `m5stick-c` |

## Forth REPL

Background task on UART0 (115200 baud). Registered words:

| Word | Action |
|------|--------|
| `wifi-ssid` | Set WiFi SSID |
| `wifi-pass` | Set WiFi password |
| `wifi-connect` | Save + connect |
| `wifi-status` | Show WiFi/MQTT/profile info |
| `wifi-clear` | Clear credentials |
| `wifi-profiles` | List/select/create profiles |
| `mqtt-broker` | Set MQTT broker URI |
| `mqtt-status` | Show MQTT connection |
| `mqtt-topic` | Show MQTT topic |
| `sound-on` | Enable chimes |
| `sound-off` | Disable chimes |
| `status` | Show device status |
| `brightness` | Set brightness (0-100) |

## Settings Screen (as implemented)

5 items, navigated with Button A (cycle) and Button B (toggle/activate):

| # | Item | Button B Action |
|---|------|----------------|
| 0 | Sound: ON/OFF | Toggle sound, persist to NVS |
| 1 | Bright: 20-100% | Cycle brightness in 20% steps |
| 2 | Prof: `<name>` | Cycle to next WiFi profile + reconnect |
| 3 | Clear: [B] | Clear all session rows |
| 4 | Back: <- | Return to WiFi screen |

## Session Row Colors (as implemented)

Background bar tints — bright enough to be clearly visible, dark enough for white text readability:

| State | Background RGB | Visual |
|-------|---------------|--------|
| IDLE | `Synth::DIM_GRAY` | Dark grey |
| WORKING | `rgb(100, 85, 0)` | Warm amber |
| NEED INPUT | `rgb(0, 50, 120)` / `rgb(0, 25, 60)` | Flashing bright blue (400ms) |
| FINISHED | `rgb(0, 90, 20)` | Vivid green |
| ERROR | `rgb(120, 15, 15)` / `rgb(50, 5, 5)` | Flashing bright red (300ms) |
| Stale (>5 min) | `Synth::DIM_GRAY` | Dimmed, no crawdad |

## Known Issues / Future Work

- **Model shows "unknown"**: Claude Code only includes the `model` field in certain hook events (Stop, SessionStart). The hook script caches per-session, but the first few messages may show "unknown" until a Stop event occurs.
- **mDNS disabled**: Not reliable in multi-subnet deployments. Component exists in `craw_mdns/` but is gated behind `CRAW_ENABLE_MDNS` cmake flag.
- **M5Dial not yet refactored**: The M5Dial project still has inline duplicated code alongside the shared component symlinks. It builds and runs, but the inline code should eventually be replaced with shared component API calls.
- **No persistent session state**: Sessions are lost on power cycle. They rebuild from live MQTT data as hooks fire.

## Open Design Questions

- **Rate limit / token exhaustion visibility**: Currently, rate limits and "out of tokens" conditions are not surfaced as a distinct state. Claude Code doesn't fire a specific hook event for these — the session simply stops receiving updates and goes stale after 5 minutes. Several options to consider:
  1. **Hook script approach**: If `session_pct` reaches 100%, the hook script could send ERROR (state=7) instead of the normal state — makes token exhaustion visible as a flashing red row.
  2. **Device-side approach**: If `session_pct >= 95%`, the device could override the row color to a warning tint (e.g. orange) even while the session is still WORKING — proactive "running low" indicator.
  3. **Staleness-as-warning approach**: If a session was WORKING and goes stale (no updates for 5 min), show it in a warning color (orange) instead of just dimming to grey — distinguishes "session ended normally" from "session stopped unexpectedly."
  4. **Combination**: Use option 2 for proactive warning + option 3 for unexpected stalls.
  
  No implementation yet — pending decision on which approach best fits the workflow.
