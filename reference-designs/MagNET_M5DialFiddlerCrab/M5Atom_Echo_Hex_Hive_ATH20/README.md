# M5Atom Echo + Unit Hex Hive Test

First MagNET hive node with both **visual status** (37 × SK6812 hex LED panel) and **audible feedback** (I2S speaker). Target bringup platform for Phase-4 Milestone A (BLE provisioning) and Milestone B (hive join). Same shared component stack as the Matrix and C3U projects, extended with a small I2S tone subsystem for state-transition chirps.

## Hardware

| | |
|---|---|
| Base | [M5Atom Echo](https://shop.m5stack.com/products/atom-echo-smart-speaker-dev-kit) — ESP32-PICO-D4 |
| Peripheral | [M5 Unit Hex](https://shop.m5stack.com/products/hex-rgb-led-board-sk6812) — 37 × SK6812 (1 center + 6 + 12 + 18 rings) |
| MCU | ESP32-PICO-D4 (classic dual-core LX6, 240 MHz) |
| Flash | 4 MB |
| SRAM | 520 KB |
| Console | UART0 @ 115200 via on-board CP2104 bridge |
| Speaker | NS4168 I2S amp + integrated speaker (BCLK=19, WS=33, DOUT=22) |
| Button | GPIO 39 (input-only, external pull-up on board) |
| Hex LED data | **GPIO 26** (Grove port white wire) |
| Radio | WiFi 802.11 b/g/n + BLE 4.2 (NimBLE, classic BT disabled) |
| PIO board | `m5stack-atom` |

## Status visualization (hex panel)

v1 uses four logical slots on the hex; the rest of the panel stays dark. Physical LED order on the Unit Hex is not documented in the wiki (index 0..36 is sequential along the internal wiring). Run `hex-test` at the Forth REPL once on hardware — each LED lights for 300 ms in index order — and adjust the `HEX_IDX_*` constants in `src/main.c` if the defaults don't land where you expect.

Defaults (provisional):

| LED index | Logical slot | Colors by state |
|---|---|---|
| 0 | hive state (center) | blue = DISCOVER, amber = CONNECTING, green = JOINED, red = BACKOFF |
| 1 | BLE | cyan = advertising, white = client connected |
| 2 | WiFi | amber = connecting, green = connected, red = failed |
| 3 | heartbeat | white flash on state transitions + button press |
| 4..36 | reserved | — (role / caps / peer count in later milestones) |

## Audio cues

Short I2S-synthesized sine tones on state transitions. All single-tone or two-tone — the speaker is not musical-grade. Adjust in `tune_*()` in `main.c` if any pattern is wrong for your environment.

| Event | Pattern |
|---|---|
| Boot | 1000 Hz 60 ms → 1500 Hz 60 ms (I2S sanity chirp) |
| BLE advertising start | 1000 Hz 80 ms |
| WiFi connected | 800 Hz 80 ms → 1200 Hz 90 ms (ascending) |
| WiFi failed | 800 Hz 80 ms → 500 Hz 120 ms (descending) |
| Hive JOINED | 1500 Hz 80 ms → 2000 Hz 100 ms (ascending) |
| Hive lost JOINED | 700 Hz 60 ms → 500 Hz 60 ms (descending) |
| Button press / heartbeat | 400 Hz 20 ms |

The tone player is backed by a FreeRTOS queue and a dedicated task, so multi-tone patterns fire from inside event callbacks without blocking the callers.

## Build and flash

```bash
pio run -e esp32 -t upload -t monitor
```

First build ≈ 50 s (full IDF compile). Incrementals are fast.

## Test plan

### 1. Boot sanity
Flash and monitor. Within a second of reset you should hear the **two-tone boot chirp** (1000 Hz → 1500 Hz) and see **LED 1 = cyan** (BLE advertising). Serial prints the BLE name as `MagNET-biologic-<MAC4>`.

If no boot chirp: I2S is mis-wired or the speaker channel didn't enable. Check UART for `ESP_ERROR_CHECK` backtraces around `i2s_new_channel` / `i2s_channel_init_std_mode`.

### 2. Physical LED mapping
At the Forth prompt:
```
ok> hex-test
```
The panel lights one LED at a time, 300 ms apart, for all 37 indices. Note which physical position is **index 0** and whether it matches the center. If not, open `src/main.c` and change `HEX_IDX_HIVE`, `HEX_IDX_BLE`, `HEX_IDX_WIFI`, `HEX_IDX_HEARTBEAT` to the indices you want to use.

### 3. BLE provisioning (Milestone A)
Open **nRF Connect** on a phone → scan → connect to `MagNET-biologic-<MAC4>`. Panel should flip **LED 1 cyan → white** (client connected) and you'll hear the BLE-advert chirp (1000 Hz).

In nRF Connect:
- Find service UUID `4d41474e-4554-0001-0000-000000000000`
- Write your SSID to characteristic `02` (UTF-8 bytes)
- Write your password to characteristic `03` (UTF-8 bytes; reads return empty by design)
- Write `01` (single byte) to characteristic `04` (`wifi_commit`)

The serial log prints `[PROV] commit -> '<ssid>'`. **LED 2** flips amber → green when WiFi comes up. You'll hear the ascending wifi-up chirp. Characteristic `05` (`ip_address`) pushes a notify with the assigned IP.

### 4. Hive join (Milestone B)
Roughly 2–3 s after WiFi, the hive node starts. **LED 0** (center) cycles **blue (DISCOVER) → amber (CONNECTING) → green (JOINED)** if a ruler is reachable, else **red (BACKOFF)** with a backoff retry.

To validate mDNS + HMAC without a Dial ruler running, use a laptop fake-ruler:

```bash
# Minimal script — save as fake-ruler.py, on same LAN as the Atom Echo
python3 fake-ruler.py --hive beehive-1 --port 7447
```

(Script not yet committed. ~150 lines: `zeroconf` service-info publish + TCP listen + cJSON/HMAC mirror of `docs/MagNET-HiveProtocol-v1.md`. Ping if you need it written now.)

### 5. REPL inspection
```
ok> prov-status
ble:   MagNET-biologic-a1b2
wifi:  connected
ssid:  MyWiFi
ip:    192.168.1.42

ok> hive-status
hive:    JOINED
node:    MagNET-biologic-a1b2
session: 3fa2-5c91-…

ok> 2000 80 beep            \ one short 2 kHz chirp

ok> 15 0 80 0 hex-pixel     \ LED 15 green

ok> prov-reset              \ clears WiFi, re-advertises
```

## Forth words

| Word | Stack | Description |
|---|---|---|
| `prov-status` | ( -- ) | Print BLE / WiFi / IP state |
| `prov-reset` | ( -- ) | Clear WiFi creds, resume BLE advertising |
| `hive-status` | ( -- ) | Print hive node state + session id |
| `hex-test` | ( -- ) | Walk every LED in index order (300 ms each) |
| `idx r g b hex-pixel` | ( i r g b -- ) | Set one hex LED (next status repaint may overwrite) |
| `freq dur-ms beep` | ( f d -- ) | Play a sine tone; max duration 2000 ms |

All standard ESPIDFORTH words also available (`words` lists them).

## Memory

| Metric | Value |
|---|---|
| Flash | 1019 KB (48.6% of 2 MB) |
| Static RAM | 110 KB (33.6% of 320 KB) |
| Forth heap | 32 KB |
| Components | `craw_nvs`, `craw_wifi`, `craw_ble_provision`, `craw_hive`, `forth`, `led_strip`, `mdns`, `esp_driver_i2s` |

## Why this project

- **Bringup target**: chosen over the Matrix project as the first end-to-end validation because it has enough pixels (37 vs 25) for richer state encoding and a speaker for audible cues — you can walk across the room and still know the node's state without reading serial.
- **First node to exercise I2S audio** in the shared stack. The `tone_cmd_t` queue + `tone_task` pattern is deliberately portable — any project that wires an I2S speaker (or LEDC buzzer with a tiny adapter) can reuse it verbatim.
- **Future role fit**: the 37-LED hex is a natural `beeper` or `spy` role display. Once Milestone C ships signed Forth role bundles, this node is a good demo of a role that actually exercises both peripherals.

## Related

- `../M5Atom_Matrix_Hive_Test/` — 5×5 matrix variant, no audio
- `../M5Stamp3CU_Blinky_E4TH/` — headless C3U variant
- `../components/craw_hive/` — shared hive protocol
- `../docs/MagNET-HiveProtocol-v1.md` — on-wire specification
