# M5Atom Matrix Hive Test

First end-to-end MagNET hive-protocol node on classic ESP32. Exercises Phase-4 Milestone A (BLE WiFi provisioning via `craw_ble_provision`) and Milestone B (mDNS discovery + HMAC join via `craw_hive`) on M5Atom Matrix hardware. Status is rendered in real time on the onboard 5×5 WS2812B panel so the behavior is glanceable without a serial monitor attached.

## Hardware

| | |
|---|---|
| Board | M5Atom Matrix |
| MCU | ESP32-PICO-D4 (classic Tensilica LX6, 2× 240 MHz) |
| Flash | 4 MB |
| SRAM | 520 KB |
| LED panel | 25 × WS2812B on **GPIO 27** (5×5, row-major, index 0 = top-left) |
| Button | **GPIO 39** (input-only, external pull-up on-board) |
| USB | UART0 via CP2104 bridge @ 115200 (no USB-serial-JTAG on classic ESP32) |
| Radio | WiFi 802.11 b/g/n + BLE 4.2 (NimBLE host, classic BT disabled to save RAM) |
| PIO board | `m5stack-atom` |

## Status visualization (5×5 matrix)

Only the top row is used in v1. Rows 1–4 are reserved for future role / caps viz once Milestone C lands role bundles.

```
   col 0     col 1     col 2     col 3     col 4
   BLE       WiFi      Hive      (spare)   heartbeat
   cyan      amber     blue      —         white flash
   white     green     green                on PING
             red       amber                & state
                       red                  transitions
```

| Column | Color | Meaning |
|---|---|---|
| 0 BLE | cyan | advertising as `MagNET-biologic-<MAC4>` |
| 0 BLE | white | a BLE client has connected (provisioning in progress) |
| 1 WiFi | amber | connecting |
| 1 WiFi | green | connected, IP assigned |
| 1 WiFi | red | failed |
| 2 Hive | blue | scanning mDNS for a ruler |
| 2 Hive | amber | TCP + HELLO in flight |
| 2 Hive | green | JOINED — session id held |
| 2 Hive | red | BACKOFF (ruler down / rejected) |
| 4 heartbeat | white | brief flash on every hive state transition and on button press |

## Build and flash

```bash
pio run -e esp32 -t upload -t monitor
```

First build takes ~50 s (full IDF component compile); incrementals are fast.

## Boot flow

1. UART0 console up, boot banner.
2. 5×5 matrix init (cleared).
3. `craw_nvs_init_flash()` + WiFi profile migration.
4. `craw_wifi_init()` registered with event callback.
5. `craw_ble_provision_init()` — starts advertising as `MagNET-biologic-<MAC4>`.
6. ESPIDFORTH engine init + 5 custom FFI words registered.
7. `ui_task` renders the status matrix every 100 ms.
8. `housekeeping_task` debounces the button and starts the hive node as soon as WiFi is up.
9. If stored WiFi creds exist → auto-connect; else → wait for BLE provisioning.

## Test plan — nRF Connect smoke test

1. Flash the firmware and open a serial monitor.
2. On the matrix you should see: **col 0 = cyan** (BLE advertising), everything else off.
3. Open nRF Connect on iOS or Android → Scanner tab → find `MagNET-biologic-<MAC4>` → Connect.
4. Matrix col 0 should flip **cyan → white** (BLE client connected).
5. In nRF Connect, expand the `4d41474e-…-00000000` service and you should see six characteristics (suffix 01 through 06). See `components/craw_ble_provision/README` style block in the E4TH README for the map.
6. Write your WiFi SSID (UTF-8) to characteristic `02` (`wifi_ssid`).
7. Write your WiFi password (UTF-8) to characteristic `03` (`wifi_pass`). Reads return empty — that is intentional.
8. Write `01` (one byte) to characteristic `04` (`wifi_commit`). The serial log should print `[PROV] commit -> '<ssid>'`.
9. Matrix col 1 flips **amber → green** when WiFi comes up. The `ip_address` characteristic (`05`) will fire a notify with the IP.
10. Roughly 3 s later the hive node starts scanning. Col 2 cycles **blue → amber → red**. Red is expected until a ruler is actually running — the Dial demo will advertise `_magnet-ruler._tcp` in the next phase. To verify the mDNS half in isolation, see the "Laptop fake-ruler" section below.

## Forth REPL

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

ok> matrix-test           \ all 25 pixels white for 400 ms

ok> 2 2 0 80 0 matrix-pixel  \ (row, col, r, g, b) → center green

ok> prov-reset            \ wipes WiFi creds, restarts BLE advertising
```

| Word | Stack | Description |
|---|---|---|
| `prov-status` | ( -- ) | Print BLE / WiFi / IP state |
| `prov-reset` | ( -- ) | Clear WiFi creds, resume BLE advertising |
| `hive-status` | ( -- ) | Print hive node state + session id |
| `matrix-test` | ( -- ) | Light every pixel briefly, then restore status |
| `row col r g b matrix-pixel` | ( r c rr gg bb -- ) | Set one pixel (next status repaint clears it) |

Plus every standard ESPIDFORTH word (`words` lists them).

## Laptop fake-ruler

Until the Dial demo is wired to speak the hive protocol, the easiest way to validate mDNS discovery + the HMAC handshake is a laptop script that advertises `_magnet-ruler._tcp` on your WiFi LAN and listens on port 7447. Script is intentionally tiny (~150 lines of Python using `zeroconf` + raw sockets + `hmac`); see the sibling project once it ships, or ask the maintainer. Plan is to check that in under `scripts/fake-ruler.py` in a follow-up commit.

Expected behavior on a successful join:
- Matrix col 2 goes **blue → amber → green**.
- Serial: `[HIVE] state=3 (<session-uuid>)`.
- Ruler's session table has a row for this node.

## Memory

| Metric | Value |
|---|---|
| Flash | 998 KB (47.6% of 2 MB) |
| Static RAM | 110 KB (33.5% of 320 KB) |
| Forth heap | 32 KB (lower than C3U 64 KB — make room for classic-BT controller) |
| Components | `craw_nvs`, `craw_wifi`, `craw_ble_provision`, `craw_hive`, `forth`, `led_strip`, `mdns` |

## Why this project exists

- **First cross-chip peer in the hive.** DigDug runs on C3, Dial demo runs on S3. This is the first node to validate the shared component tree on classic ESP32, which is a meaningful smoke test for chip-specific regressions.
- **Glanceable test rig.** The 5×5 matrix replaces three `printf` lines with three colored pixels — faster feedback loop when iterating on the protocol layer.
- **Future role target.** An M5Atom Matrix is a natural fit for the `beeper` (cols = alert level), `spy` (scan LEDs light up when a new node is seen), or `pet` (idle animations) roles once Milestone C ships.

## Related

- `../M5Stamp3CU_Blinky_E4TH/` — same protocol stack on ESP32-C3 (no display)
- `../M5Stamp_DigDug_BLE_Lighting/` — BLE lighting (original source for `craw_ble_provision`)
- `../components/craw_hive/` — shared hive protocol component
- `../docs/MagNET-HiveProtocol-v1.md` — on-wire specification
