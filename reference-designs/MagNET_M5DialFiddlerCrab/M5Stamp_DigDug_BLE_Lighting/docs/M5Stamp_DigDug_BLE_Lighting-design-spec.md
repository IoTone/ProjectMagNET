# M5Stamp_DigDug_BLE_Lighting — Design Spec

## Overview

A BLE-controlled smart light on the M5StampC3U (ESP32-C3). Advertises as a controllable light over BLE GATT with characteristics for power, brightness, color (RGB), and ramp rate. Also exposes the same controls over HTTP for WiFi-based access. The device's IP address is published via a BLE characteristic so BLE clients can discover the HTTP endpoint without mDNS.

Uses shared `craw_*` components for WiFi, NVS (WiFi profiles), and serial I/O. Uses ESPIDFORTH for interactive scripting with hybrid BLE/LED Forth words. All application logic is in `src/main.c` (~960 lines).

Ported from the M5Atom_DigDug_Camera_Button BLE concept, targeting the M5StampC3U hardware.

## Hardware: M5StampC3U

| Spec | Value |
|------|-------|
| SoC | ESP32-C3 (RISC-V, single-core 160MHz) |
| Flash | 4 MB |
| PSRAM | None |
| LED | SK6812 (addressable RGB) on **GPIO 2** (via RMT + `espressif/led_strip`) |
| Button | **GPIO 9** (active low, internal pull-up) |
| USB | Native USB-serial-JTAG |
| BLE | BLE 5.0 via NimBLE (ESP-IDF built-in) |
| WiFi | 802.11 b/g/n (2.4 GHz) |
| PIO Board | `esp32-c3-devkitc-02` |

## Architecture

```
┌────────────────────────────────┐
│         Forth REPL             │  Interactive scripting (18 words)
│  led-on led-off led-rgb       │  LED control words
│  ble-status ble-advertise     │  BLE control words
│  wifi-ssid wifi-connect       │  WiFi words (via craw_wifi)
│  versions status save         │  System words
├────────────────────────────────┤
│      BLE GATT Server          │  NimBLE: 8 characteristics
│  Power|Bright|R|G|B|Ramp|IP|Name
├───────────┬────────────────────┤
│ HTTP API  │  craw_wifi         │  /light/status, /light/set, /ping
│ (direct)  │  craw_nvs (WiFi)   │  WiFi profiles + credentials
├───────────┴────────────────────┤
│  LED Driver (RMT + led_strip)  │  SK6812 on GPIO 2
│  NVS (direct, "digdug_light")  │  Light state persistence
│  craw_serial                   │  USB-serial-JTAG abstraction
└────────────────────────────────┘
```

### Key design decisions

- **BLE in main.c** — the GATT service is lighting-specific, not reusable. Keep it in main.c rather than a separate component.
- **HTTP via esp_http_server directly** — `craw_http` depends on `craw_mqtt` which we don't need. Use the raw ESP-IDF HTTP server API for the 3 light endpoints.
- **Light NVS in separate namespace** `"digdug_light"` — independent from `craw_nvs`'s `"craw_config"` namespace used for WiFi profiles.
- **Forth heap: 32KB** — reduced from 48KB due to WiFi + BLE RAM pressure on C3.
- **NVS writes debounced** — esp_timer one-shot (2s delay) prevents flash wear from rapid BLE slider changes.

## BLE GATT Service

Service UUID: `dd841d1c-869e-48b6-b882-924fd3276600` (128-bit custom)

Device advertises as `"DigDug-XXXX"` (last 4 hex of MAC address).

### Characteristics

| Characteristic | UUID suffix | Properties | Type | Description |
|---|---|---|---|---|
| Power | ...01 | Read/Write | uint8 | 0=off, 1=on |
| Brightness | ...02 | Read/Write | uint8 | 0-255 |
| Color R | ...03 | Read/Write | uint8 | Red 0-255 |
| Color G | ...04 | Read/Write | uint8 | Green 0-255 |
| Color B | ...05 | Read/Write | uint8 | Blue 0-255 |
| Ramp Rate | ...06 | Read/Write | uint16 | Transition time in ms (0-5000) |
| IP Address | ...07 | Read/Notify | string | IPv4 address or "N/A" |
| Device Name | ...08 | Read | string | Hostname (e.g. "DigDug-a1b2") |

### BLE behavior
- On characteristic write → LED updates immediately, debounced NVS save scheduled
- IP Address characteristic sends notification when WiFi connects or disconnects
- GAP advertising restarts automatically on client disconnect
- Connectable undirected advertising, indefinite duration
- Max 3 simultaneous connections

## HTTP API

Starts automatically after WiFi connects. Three endpoints on port 80:

| Method | Path | Description |
|--------|------|-------------|
| GET | `/light/status` | JSON with all light state fields |
| GET | `/light/set` | Set light parameters via query string |
| GET | `/ping` | Returns `"pong"` (health check) |

The `/light/set` endpoint accepts any subset of parameters — only the provided values change.

### Finding the device IP

The device does not use mDNS. To find the IP address:

1. **Serial REPL**: type `wifi-status` — prints IP address
2. **BLE**: connect with nRF Connect or LightBlue, read the "IP Address" characteristic (UUID ...07)
3. **Serial monitor on boot**: prints `WiFi connected: <ip>` when it joins the network

### Usage examples (curl)

```bash
# Turn on (white, current brightness)
curl "http://192.168.1.50/light/set?power=1"

# Turn on with a specific color
curl "http://192.168.1.50/light/set?power=1&r=255&g=0&b=0"

# Set color + brightness + ramp in one call
curl "http://192.168.1.50/light/set?power=1&r=0&g=255&b=128&brightness=200&ramp=500"

# Dim to 25%
curl "http://192.168.1.50/light/set?brightness=64"

# Turn off
curl "http://192.168.1.50/light/set?power=0"

# Check current state
curl "http://192.168.1.50/light/status"
# → {"power":1,"r":0,"g":255,"b":128,"brightness":200,"ramp":500}

# Health check
curl "http://192.168.1.50/ping"
# → pong
```

### Integration examples

**Shell script — flash red for 2 seconds:**
```bash
#!/bin/bash
IP="192.168.1.50"
curl -s "http://$IP/light/set?power=1&r=255&g=0&b=0&brightness=255"
sleep 2
curl -s "http://$IP/light/set?power=0"
```

**Claude Code hook — flash light on NEED_INPUT:**
```bash
# In your hook script, after detecting state=3 (NEED_INPUT):
curl -s "http://$DIGDUG_IP/light/set?power=1&r=0&g=100&b=255&brightness=255" &
```

**Cron job — nightlight at sunset:**
```bash
# crontab: 0 20 * * * /path/to/nightlight.sh
curl -s "http://192.168.1.50/light/set?power=1&r=255&g=140&b=30&brightness=80&ramp=3000"
```

**Home Assistant REST command:**
```yaml
rest_command:
  digdug_on:
    url: "http://192.168.1.50/light/set?power=1&r={{ r }}&g={{ g }}&b={{ b }}&brightness={{ brightness }}"
  digdug_off:
    url: "http://192.168.1.50/light/set?power=0"
```

**From another ESP32 (esp_http_client):**
```c
esp_http_client_config_t cfg = { .url = "http://192.168.1.50/light/set?power=1&r=255&g=0&b=0" };
esp_http_client_handle_t client = esp_http_client_init(&cfg);
esp_http_client_perform(client);
esp_http_client_cleanup(client);
```

### Response format

`/light/status` returns JSON:
```json
{
  "power": 1,
  "r": 255,
  "g": 0,
  "b": 128,
  "brightness": 200,
  "ramp": 500
}
```

`/light/set` returns `"OK"` on success.

`/ping` returns `"pong"`.

## LED Driver

Uses the `espressif/led_strip` managed component with RMT backend (ported from M5Stamp3CU_Blinky_E4TH):
- 1 SK6812 LED on GPIO 2
- 10 MHz RMT clock resolution
- Brightness scaling: `actual_r = (light.r * light.brightness) / 255`
- When `power=0`, LED is set to 0,0,0 (off)
- Ramp rate: smooth transition via esp_timer interpolation (linear RGB blend over `ramp_ms`)

## NVS Persistence

### Light settings (namespace: `"digdug_light"`)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `power` | u8 | 0 | Power state |
| `bright` | u8 | 200 | Brightness |
| `r` | u8 | 255 | Red |
| `g` | u8 | 255 | Green |
| `b` | u8 | 255 | Blue |
| `ramp` | u16 | 0 | Ramp rate ms |

Write debouncing: on each `light_state_changed()`, a 2-second one-shot timer is (re)started. When it fires, all 6 values are written to NVS in one batch. Rapid changes (e.g. BLE slider drag) result in at most 1 NVS write per 2 seconds.

### WiFi settings (namespace: `"craw_config"`, via craw_nvs)

Standard WiFi profile management — same as all craw_* projects. Profiles, credentials, active profile stored via `craw_nvs` shared component.

## Forth REPL (Hybrid)

Background-blocking REPL on USB-serial-JTAG (115200 baud via `craw_serial`). C layer handles BLE/LED hardware init; Forth words provide interactive control of BLE characteristics and LED state.

### Registered Words (18 total)

| Word | Stack | Description |
|------|-------|-------------|
| `led-on` | ( -- ) | Power on with current color |
| `led-off` | ( -- ) | Power off |
| `led-rgb` | ( r g b -- ) | Set color + power on |
| `led-brightness` | ( n -- ) | Set brightness 0-255 |
| `led-ramp` | ( n -- ) | Set ramp rate in ms |
| `led-status` | ( -- ) | Print current light state |
| `ble-status` | ( -- ) | Print BLE connection info |
| `ble-advertise` | ( -- ) | Restart BLE advertising |
| `wifi-ssid` | ( -- ) | Prompt for WiFi SSID |
| `wifi-pass` | ( -- ) | Prompt for WiFi password |
| `wifi-connect` | ( -- ) | Save + connect to WiFi |
| `wifi-status` | ( -- ) | Print WiFi/IP info |
| `wifi-clear` | ( -- ) | Clear WiFi credentials |
| `versions` | ( -- ) | Print all component versions |
| `status` | ( -- ) | Print full device status |
| `save` | ( -- ) | Force NVS save of light state |

## Button

| Action | Behavior |
|--------|----------|
| Short press | Toggle power on/off |
| Long press (5s) | Clear WiFi credentials + reboot |

GPIO 9, active low, ISR on positive edge (release), debounced via FreeRTOS task.

## Boot Splash

```
╔══════════════════════════════════════╗
║  M5Stamp DigDug BLE Lighting v0.1.0 ║
║  IoTone, Inc. 2026                   ║
╠══════════════════════════════════════╣
║  craw_serial  v0.1.0                 ║
║  craw_wifi    v0.1.0                 ║
║  craw_nvs     v0.1.0                 ║
║  ESPIDFORTH   v0.2.0                 ║
║  ESP-IDF      v5.3.1                 ║
╚══════════════════════════════════════╝
Hardware: M5StampC3U (ESP32-C3, 4MB)
LED: SK6812 on GPIO 2
BLE: NimBLE (advertising as DigDug-XXXX)
```

Version strings read from component `#define`s at compile time.

## Shared Components

| Component | Version | Used | Purpose |
|-----------|---------|------|---------|
| `craw_serial` | 0.1.0 | Yes | USB-serial-JTAG I/O for Forth REPL |
| `craw_wifi` | 0.1.0 | Yes | WiFi STA with event callbacks |
| `craw_nvs` | 0.1.0 | Yes | WiFi profile/credential management |
| `craw_http` | 0.1.0 | No | Depends on craw_mqtt; use esp_http_server directly |
| `craw_mqtt` | 0.1.0 | No | No MQTT needed |
| `craw_speaker` | 0.1.0 | No | No speaker on M5StampC3U |
| `craw_mdns` | 0.1.0 | No | mDNS unreliable; IP via BLE characteristic |
| `forth` | 0.2.0 | Yes | ESPIDFORTH interpreter |

Components are symlinked from `../components/`. The `forth` component is copied from `ESPIDFORTH/components/forth/`.

## Build Results

| Metric | Value |
|--------|-------|
| Flash | 979 KB (46.7% of 2MB partition) |
| RAM | 94 KB (28.8% of 328 KB) |
| Forth heap | 32 KB |
| Platform | espressif32@6.9.0, ESP-IDF 5.3.1 |
| Board | `esp32-c3-devkitc-02` |

## Boot Sequence

1. `craw_serial_init()` — USB-serial-JTAG
2. Print boot splash with all component versions
3. `led_init()` — RMT + led_strip for SK6812 on GPIO 2
4. `btn_init()` — GPIO 9 ISR + button task
5. `craw_nvs_init_flash()` + `craw_nvs_migrate_wifi_profiles()`
6. `light_nvs_load()` — restore saved light state
7. `apply_light_state()` — show saved color immediately on LED
8. NVS debounce timer creation
9. `derive_hostname()` — "DigDug-XXXX" from MAC
10. `craw_wifi_init()` — WiFi STA with event callback
11. `ble_init()` — NimBLE GATT server, start advertising
12. `forth_init()` + register 18 words
13. Auto-connect WiFi from NVS (if credentials stored)
14. `forth_repl()` — blocks forever, interactive REPL

## Testing

| # | Test | Expected |
|---|------|----------|
| 1 | Boot | Splash with versions prints to serial |
| 2 | `255 0 0 led-rgb` | LED turns red |
| 3 | `led-off` / `led-on` | LED off, then restores red |
| 4 | `128 led-brightness` | LED dims to half |
| 5 | nRF Connect → scan | See "DigDug-XXXX" |
| 6 | nRF Connect → write brightness=64 | LED dims further |
| 7 | nRF Connect → read IP Address | "N/A" (no WiFi yet) |
| 8 | `wifi-ssid` + `wifi-pass` + `wifi-connect` | Connects, IP appears in BLE |
| 9 | `curl http://<ip>/light/status` | JSON response |
| 10 | `curl "http://<ip>/light/set?r=0&g=255&b=0"` | LED turns green |
| 11 | Power cycle | LED restores saved color |
| 12 | `versions` | All component versions |
| 13 | Short button press | Toggle power |
| 14 | Long button press (5s) | WiFi clear + reboot |

## Performance Debug Mode

A built-in performance timing system measures latency of all lighting operations. Defaults to **off** (zero overhead). Toggle via Forth REPL:

```forth
debug-on     \ enable timing output
debug-off    \ disable timing output
```

### What's instrumented

| Instrumentation Point | What it measures |
|----------------------|-----------------|
| `apply_light_state` | RMT LED pixel write latency (GPIO → photon) |
| `light_state_changed (total)` | LED write + NVS debounce timer schedule |
| `HTTP /light/status` | Full HTTP request → JSON response |
| `HTTP /light/set` | Full HTTP request → LED update → response |

BLE GATT write operations are measured indirectly via `light_state_changed` — the BLE stack overhead above that is logged by NimBLE at DEBUG log level if needed.

### Output format

When `debug-on` is active, each instrumented operation prints to the serial monitor:

```
I (12345) digdug: [PERF] apply_light_state: 45 us (0.05 ms)
I (12345) digdug: [PERF] light_state_changed (total): 52 us (0.05 ms)
```

For HTTP requests:
```
I (12345) digdug: [PERF] apply_light_state: 43 us (0.04 ms)
I (12345) digdug: [PERF] light_state_changed (total): 50 us (0.05 ms)
I (12345) digdug: [PERF] HTTP /light/set: 320 us (0.32 ms)
```

### Implementation

Uses microsecond-precision `esp_timer_get_time()` with zero-cost macros when disabled:

```c
static bool debug_perf = false;

#define PERF_START()    int64_t _perf_t0 = debug_perf ? esp_timer_get_time() : 0
#define PERF_END(label) do { \
    if (debug_perf) { \
        int64_t _dt = esp_timer_get_time() - _perf_t0; \
        ESP_LOGI(TAG, "[PERF] %s: %lld us (%.2f ms)", (label), _dt, _dt / 1000.0); \
    } \
} while(0)
```

When `debug_perf == false`, `PERF_START()` evaluates to `int64_t _perf_t0 = 0` (optimized away by compiler) and `PERF_END()` short-circuits on the `if` check. No measurable overhead in production.

## Network Latency Discussion: Transport Protocol Comparison

This section analyzes the expected latency and reliability of different transport protocols for IoT light control. The payload is small — a single control packet is typically 10-50 bytes (e.g., `power=1&r=255&g=0&b=0`). The question is: what's the fastest and most reliable way to get that packet from a controller to the LED?

### Protocol Comparison

| Protocol | Expected Latency | Reliability | Connection | Notes |
|----------|-----------------|-------------|------------|-------|
| **BLE 5.0 (direct)** | **1-5 ms** | Very high | Persistent | Lowest latency. No network infrastructure needed. |
| **UDP over WiFi 5 (2.4GHz)** | **2-10 ms** | Medium | Connectionless | Very fast but no delivery guarantee. Packets can drop silently. |
| **HTTP over WiFi 5 (2.4GHz)** | **10-50 ms** | High | Per-request TCP | Current implementation. TCP handshake + HTTP overhead. Reliable but slowest WiFi option. |
| **MQTT over WiFi 5 (2.4GHz)** | **5-20 ms** | High | Persistent TCP | Persistent connection avoids TCP handshake per message. QoS 1 guarantees delivery. |
| **WebSocket over WiFi 5 (2.4GHz)** | **3-10 ms** | High | Persistent TCP | Best of both: persistent connection + full-duplex. Requires external server. |

### Detailed Analysis

#### BLE 5.0 Direct — Lowest Latency (~1-5 ms)

BLE GATT write-without-response is the fastest path. The BLE connection interval (typically 7.5-30 ms on iOS/Android, configurable down to 7.5 ms) determines the minimum latency floor. A GATT write lands in the next connection event.

**Advantages:**
- No WiFi infrastructure required (works anywhere, no router)
- No network stack overhead (no TCP/IP, no DNS, no HTTP parsing)
- Sub-10ms response time for characteristic writes
- Power efficient (BLE 5.0 uses ~10 mA during active connection)
- Encrypted by default (BLE pairing)

**Disadvantages:**
- Range limited to ~10-30 meters (line of sight, ~5-10m through walls)
- Requires BLE-capable controller (phone app, another ESP32, Raspberry Pi)
- Max 3 simultaneous connections (NimBLE configured limit)
- No built-in bridge to IP networks (can't control from a remote server)

**Best for:** Direct control from a nearby phone or dedicated controller. Lowest possible latency for interactive use (color pickers, sliders, button toggles).

#### UDP over WiFi — Fastest WiFi (~2-10 ms)

Raw UDP packets carry minimal overhead — no connection establishment, no acknowledgment, no ordering. A 50-byte control packet fits in a single WiFi frame.

**Advantages:**
- Extremely low latency (no TCP handshake, no HTTP parsing)
- Works across any IP network (local or remote via port forwarding)
- Very low CPU and memory overhead on the ESP32
- Can support multicast (one packet controls multiple devices)

**Disadvantages:**
- **No delivery guarantee** — packets drop silently under WiFi congestion, interference, or brief signal loss
- No built-in retry or acknowledgment (application must implement if needed)
- Firewalls and NAT often block UDP
- No standard tooling (can't test with `curl`, need custom sender)
- Security: no encryption unless application-layer TLS/DTLS added

**Best for:** High-frequency, loss-tolerant updates (e.g., streaming color data at 30+ fps for music visualization). Not suitable for critical on/off control where a dropped packet means the light stays in the wrong state.

#### MQTT over WiFi — Best Balance (~5-20 ms)

MQTT maintains a persistent TCP connection to a broker. Messages are small (topic + payload, typically <100 bytes). QoS 1 guarantees at-least-once delivery with a single PUBACK round-trip.

**Advantages:**
- Persistent TCP connection eliminates per-message handshake overhead
- QoS levels (0=fire-and-forget, 1=at-least-once, 2=exactly-once)
- Broker decouples sender and receiver (works across networks, through NAT)
- Retained messages: device gets last-known-good state on reconnect
- Wildcard subscriptions for multi-device control
- Well-supported ecosystem (Mosquitto, HiveMQ, AWS IoT, etc.)

**Disadvantages:**
- Requires a broker (local Mosquitto or cloud service)
- Broker adds a hop: controller → broker → device (~2× the base latency)
- Public brokers (HiveMQ) add internet RTT (~20-100ms depending on geography)
- TCP keepalive and reconnection logic needed for reliability
- QoS 2 (exactly-once) adds significant latency (~3× QoS 0)

**Latency breakdown (local broker):**
- Controller → broker: ~1-3 ms (local network)
- Broker → device: ~1-3 ms (local network)
- Total: ~5-10 ms with QoS 0, ~8-15 ms with QoS 1

**Latency breakdown (cloud broker):**
- Controller → broker: ~20-50 ms (internet RTT)
- Broker → device: ~20-50 ms (internet RTT)
- Total: ~40-100 ms

**Best for:** Multi-device control, cross-network access, integration with home automation systems. The standard choice for IoT when BLE range is insufficient.

#### WebSocket over WiFi — Best Persistent WiFi (~3-10 ms)

A WebSocket connection upgrades a single HTTP request to a persistent, full-duplex TCP stream. Once established, messages flow in both directions with minimal framing overhead (~2-6 bytes per message).

**Advantages:**
- Persistent connection: no per-message TCP handshake
- Full-duplex: device can push state changes back to the controller
- Lower per-message overhead than HTTP (~6 bytes vs ~200+ bytes of HTTP headers)
- Works through firewalls and proxies (uses port 80/443)
- Can use TLS (wss://) for encryption

**Disadvantages:**
- Requires an external WebSocket server (the ESP32 is typically the client, connecting outbound)
- Server adds complexity and a dependency
- Connection management: reconnection logic, heartbeats
- More RAM than UDP or MQTT (~2-4 KB per connection for buffers)
- Not as widely supported in IoT ecosystems as MQTT

**Best for:** Real-time bidirectional communication when you control the server infrastructure. Good for custom dashboards or apps that need instant feedback.

#### HTTP over WiFi — Simplest, Highest Latency (~10-50 ms)

The current implementation. Each request is a full TCP connection: SYN → SYN-ACK → ACK → HTTP GET → Response → FIN.

**Advantages:**
- Simplest to implement and debug (`curl` from any terminal)
- Works with any HTTP client (browsers, scripts, Home Assistant, IFTTT)
- Stateless: no connection management, no reconnection logic
- Easy to secure with HTTPS (ESP-IDF supports mbedTLS)

**Disadvantages:**
- **Highest latency** of all WiFi options due to per-request TCP handshake
- ~200+ bytes of HTTP headers for a 50-byte payload (4:1 overhead)
- No push: device can't notify the controller of state changes
- Each request consumes a socket and ESP-IDF httpd worker thread

**Latency breakdown:**
- TCP handshake: ~2-5 ms (local network, 3-way handshake)
- HTTP request + response: ~5-15 ms (parsing, handler, JSON serialization)
- Total: ~10-30 ms typical, ~50 ms worst case under load

**Best for:** Simple integrations, debugging, scripting. The "it just works" option when latency isn't critical.

### Recommendation Matrix

| Use Case | Recommended Protocol | Why |
|----------|---------------------|-----|
| Phone app controlling a single light | **BLE 5.0** | Lowest latency, no infrastructure |
| Multiple lights in a room | **MQTT (local broker)** | One publish controls all subscribers |
| Remote access (outside home) | **MQTT (cloud broker)** | Works through NAT, no port forwarding |
| Music-reactive lighting | **UDP** | Highest throughput, tolerates drops |
| Home Assistant integration | **MQTT** or **HTTP** | Both well-supported in HA |
| Custom real-time dashboard | **WebSocket** | Bidirectional, low overhead |
| Quick testing and debugging | **HTTP** | `curl` from terminal, zero setup |
| Battery-powered controller | **BLE 5.0** | Lowest power consumption |

### What This Project Implements

Currently: **BLE 5.0** (primary) + **HTTP** (secondary, after WiFi connect).

The `debug-on` Forth word enables performance timing to measure actual latency on your network. Use it to decide whether HTTP is sufficient or whether MQTT/WebSocket should be added for your specific deployment.

### Future Protocol Options

Adding MQTT or WebSocket would require:
- **MQTT**: Symlink `craw_mqtt` component, subscribe to a control topic, add message handler (~50 lines). The infrastructure already exists in the craw_mqtt component.
- **WebSocket**: Use ESP-IDF's `esp_websocket_client` component. Connect to an external WS server, send/receive JSON control messages. ~100 lines of new code.
- **UDP**: Use raw `lwip/sockets.h` with `recvfrom()`/`sendto()`. Minimal code (~30 lines) but needs a custom sender. No existing component.

## Forth Scripting Examples

### Timing words

Two timing primitives are registered as Forth words:

| Word | Stack | Description |
|------|-------|-------------|
| `ms` | ( n -- ) | Delay n milliseconds (max 30000) |
| `millis` | ( -- n ) | Push current uptime in ms |

### Rainbow show (5 seconds)

Define a word that cycles through colors, then run it:

```forth
: rainbow
  255 0 0 led-rgb 200 ms
  255 128 0 led-rgb 200 ms
  255 255 0 led-rgb 200 ms
  0 255 0 led-rgb 200 ms
  0 255 255 led-rgb 200 ms
  0 0 255 led-rgb 200 ms
  128 0 255 led-rgb 200 ms
  255 0 255 led-rgb 200 ms
  255 0 0 led-rgb 200 ms
  0 0 0 led-rgb 200 ms
  255 128 0 led-rgb 200 ms
  0 255 0 led-rgb 200 ms
  0 0 255 led-rgb 200 ms
  255 0 255 led-rgb 200 ms
  255 255 0 led-rgb 200 ms
  0 255 255 led-rgb 200 ms
  128 0 255 led-rgb 200 ms
  255 0 0 led-rgb 200 ms
  0 255 0 led-rgb 200 ms
  255 128 0 led-rgb 200 ms
  255 0 0 led-rgb 200 ms
  0 255 128 led-rgb 200 ms
  128 0 255 led-rgb 200 ms
  255 255 0 led-rgb 200 ms
  0 128 255 led-rgb 200 ms
  led-off
;
rainbow
```

25 steps × 200ms = 5 seconds of rainbow, then off. Type `rainbow` anytime to replay.

### Loop-based rainbow (using DO...LOOP)

```forth
: rainbow-loop
  50 0 do
    i 5 * 255 mod
    i 5 * 85 + 255 mod
    i 5 * 170 + 255 mod
    led-rgb
    100 ms
  loop
  led-off
;
```

50 iterations × 100ms = 5 seconds. Uses modular arithmetic to cycle RGB values.

### Breathing effect

```forth
: breathe
  20 0 do
    i 12 * led-brightness 100 ms
  loop
  20 0 do
    255 i 12 * - led-brightness 100 ms
  loop
;
```

Ramps brightness up then down over 4 seconds.

### Police strobe

```forth
: police
  10 0 do
    255 0 0 led-rgb 100 ms
    0 0 255 led-rgb 100 ms
  loop
  led-off
;
```

Alternates red/blue for 2 seconds.

## Forth Code Persistence — Design Options

Forth word definitions typed at the REPL live in memory and are lost on reboot. Three approaches for persistence:

### Option A: Embedded Forth file (recommended for production)

Store a `.fs` file in the project source, embed it into the binary via CMakeLists.txt `EMBED_FILES`, and `forth_eval()` it at boot.

```
src/
  main.c
  init.fs     ← Forth definitions loaded at boot
```

In CMakeLists.txt:
```cmake
idf_component_register(
    SRCS "main.c"
    EMBED_FILES "init.fs"
    ...
)
```

In main.c:
```c
extern const char init_fs_start[] asm("_binary_init_fs_start");
extern const char init_fs_end[]   asm("_binary_init_fs_end");
// ...
forth_eval_n(init_fs_start, init_fs_end - init_fs_start);
```

**Pros**: Definitions always available, no NVS space used, version-controlled with the project.
**Cons**: Requires reflash to change definitions.

### Option B: NVS-stored Forth

Add `save-forth` and `load-forth` words that store/retrieve a Forth source string in NVS. The user types definitions at the REPL, then `save-forth` persists them. On boot, `load-forth` replays them.

```forth
: rainbow 255 0 0 led-rgb 200 ms 0 255 0 led-rgb 200 ms 0 0 255 led-rgb 200 ms led-off ;
save-forth
```

**Pros**: No reflash needed, interactive development persists across reboots.
**Cons**: NVS blob size limited (~4KB practical), no editor — must type perfectly or re-enter. No version control.

### Option C: SPIFFS/LittleFS partition

A filesystem partition for `.fs` files. Upload files via serial or HTTP. Most flexible.

```
: upload-file  ( -- )  \ reads serial data into a file
: run-file     ( addr len -- )  \ evaluates a .fs file from filesystem
```

**Pros**: Multiple files, easy to update, large storage.
**Cons**: Requires a flash partition (~64KB minimum), filesystem overhead, upload tooling.

### Recommendation

Start with **Option A** for known-good definitions (rainbow, breathe, etc.) that ship with the firmware. Add **Option B** later for user-defined words that persist without reflashing. Option C is overkill for a single-LED device.

## Related Projects

| Project | Device | Purpose |
|---------|--------|---------|
| M5Atom_DigDug_Camera_Button | M5Atom | Original BLE button PoC (Arduino, ArduinoBLE) |
| M5Stamp3CU_Blinky_E4TH | M5StampC3U | Forth + LED patterns (reference for LED driver) |
| M5Stamp3CU_Blinky_Claw_OpenWR | M5StampC3U | WiFi + MQTT + HTTP (reference for networking) |
| M5StackDial-Blinky_Claw_OpenWR | M5Dial | Single-session Claude Code display |
| M5StickCPlus-Blinky_Crawdad_OpenWR | M5StickC Plus | Multi-session Claude Code dashboard |
