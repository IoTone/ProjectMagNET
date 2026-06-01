# M5 Hive Camera

OV2640 camera as a MagNET hive node. Preserves stock ESP32-CameraWebServer HTTP behavior (`/stream`, `/capture`, `/control`, `/status`) **and** adds BLE provisioning + mDNS discovery + HMAC-authenticated hive join.

Single source tree, two PlatformIO envs, one shared `craw_camera` component. Built on the same reference-design stack validated in `M5Atom_Echo_Hex_Hive_Test/` — post-WiFi bringup sequence is identical: BLE teardown → SNTP → hive start.

## Targets

| Env | Board | MCU | Flash | PSRAM | Console |
|---|---|---|---|---|---|
| `esp32cam` | AI-Thinker ESP32-CAM | ESP32 classic | 4 MB | 4 MB QSPI | UART0 via external USB-TTL adapter |
| `m5cams3` | [M5Stack Unit CamS3](https://docs.m5stack.com/en/unit/Unit-CamS3) | ESP32-S3 | 8 MB | 8 MB OPI | native USB-serial-JTAG |

## Pin maps

In `components/craw_camera/pins_ai_thinker.h` and `pins_m5cams3.h`. Standard AI-Thinker pins verified against the Arduino reference. M5CamS3 pins from M5 docs — verify on your unit if sensor init fails (M5 has shipped minor revisions).

## Build

```bash
# AI-Thinker
pio run -e esp32cam

# M5Stack Unit CamS3
pio run -e m5cams3
```

Sizes (as of first clean build):

| Env | Flash (of 2.5 MB app partition) | Static RAM | App state (post-BLE-teardown) |
|---|---|---|---|
| esp32cam | 1123 KB (44%) | 118 KB | ~60 KB with BLE torn down; frame buffers in 4 MB PSRAM |
| m5cams3  | 1047 KB (41%) | 112 KB | ~55 KB; frame buffers in 8 MB PSRAM |

## Flash

### ESP32-CAM (AI-Thinker)

Programming requires an external USB-TTL adapter (FTDI, CP2102, CH340) — there's no bridge on the board. Standard wiring:

```
USB-TTL     ESP32-CAM
GND    →    GND
5V     →    5V
TX     →    U0R
RX     →    U0T
```

**Boot-mode dance** to enter flash mode:
1. Short IO0 to GND (or press the on-board IO0 button if your revision has one)
2. Press RST
3. Release RST
4. Release IO0

Then:
```bash
pio run -e esp32cam -t upload
pio run -e esp32cam -t monitor
```

Press RST after upload to jump to the new firmware.

### Unit CamS3

Plug in USB-C directly to your laptop. The ESP32-S3's native USB exposes a serial port — no adapter, no boot-mode dance, just flash and monitor:

```bash
pio run -e m5cams3 -t upload -t monitor
```

## Test workflow

1. **Flash**, open the serial monitor. Expect boot banner, camera init line showing board name + flash GPIO, then `BLE: MagNET-biologic-<MAC4>`.
2. **Open nRF Connect on phone** → Scanner → find `MagNET-biologic-<MAC4>` → Connect.
3. **In the service list**, find service UUID starting `4d41474e-4554-0001-…` (MAGNET in ASCII). Write WiFi SSID as UTF-8 bytes to char `…002`, password to `…003`, single byte `01` to `…004` to commit.
4. **Serial log** prints `[WiFi] connected, IP: <addr>` → `[BLE] torn down +~55 KB` → `[SNTP] time synced` → `[HTTP] stream: http://magnet-cam-<mac4>.local/stream` → `[HIVE] node started as role=spy caps=camera,jpeg`.
5. **In a browser on the same LAN**, open `http://magnet-cam-<mac4>.local/` — you should see the live stream. `/capture` returns a single JPEG. `/control?var=quality&val=10` changes JPEG quality. `/status` returns JSON.
6. **On the hive ruler** (Dial or Python fake ruler), a `ruler-status` or log line shows `MagNET-biologic-<mac4>` joined as `spy` with caps `["camera","jpeg"]`.

## Forth words

| Word | Stack | Description |
|---|---|---|
| `cam-snap` | ( -- size ) | Capture one JPEG; prints `JPEG <N> bytes <WxH>`; pushes size onto the Forth stack |
| `cam-quality` | ( q -- ) | Set JPEG quality 0–63 (lower = better). Persisted to NVS |
| `cam-framesize` | ( fs -- ) | Set frame size enum (see table below). Persisted to NVS |
| `cam-vflip` | ( n -- ) | 1 = vertical flip (upside-down), 0 = off. Persisted to NVS |
| `cam-hmirror` | ( n -- ) | 1 = horizontal mirror, 0 = off. Persisted to NVS |
| `cam-reset-settings` | ( -- ) | Wipe persisted camera settings from NVS. Takes effect on next boot. Does **not** touch WiFi creds |
| `cam-xclk-mhz` | ( mhz -- ) | Persist XCLK override (4–24 MHz). Takes effect on next boot. Use 10 or 8 if you see `NO-SOI` / frame-timeout errors on AI-Thinker |
| `flash-on` / `flash-off` | ( -- ) | Toggle bright LED (AI-Thinker GPIO 4) or status LED (CamS3 GPIO 14) |
| `stream-url` | ( -- ) | Print the two URLs you can hit from a browser |
| `prov-status` | ( -- ) | BLE / WiFi / SSID / IP / SNTP / hostname / board / camera / hive state |
| `prov-reset` | ( -- ) | Clear WiFi creds and reboot into BLE provisioning mode |
| `hive-status` | ( -- ) | Print hive node state + session id |

All standard ESPIDFORTH words are available too — use `words` to list them.

### Framesize values

`cam-framesize` takes an integer from the table below. Firmware default on a fresh boot is **8 (VGA 640×480)** — chosen as the stability sweet spot on the AI-Thinker board where larger frames correlate with PSRAM bandwidth saturation and power-supply brownout artifacts (color flashes, dropped frames). Values are persisted to NVS the moment you set them, so after `10 cam-framesize` the board comes back up at XGA on every subsequent boot until you say otherwise.

| Value | Name | Resolution | JPEG typical size @ q12 |
|---|---|---|---|
| 0 | QQVGA | 160×120 | ~2 KB |
| 3 | HQVGA | 240×176 | ~3 KB |
| 5 | QVGA | 320×240 | ~6 KB |
| 6 | CIF | 400×296 | ~10 KB |
| 7 | HVGA | 480×320 | ~14 KB |
| **8** | **VGA** | **640×480** | **~20 KB — default** |
| 9 | SVGA | 800×600 | ~32 KB |
| 10 | XGA | 1024×768 | ~55 KB |
| 12 | HD | 1280×720 | ~70 KB |
| 13 | SXGA | 1280×1024 | ~90 KB |
| 14 | UXGA | 1600×1200 | ~130 KB |

**Stability guidance:**

- **QVGA / VGA (5 / 8)**: rock-solid on both AI-Thinker and CamS3 even with weak USB power. Choose this if you see color flashes or frame drops.
- **SVGA / XGA (9 / 10)**: fine on a clean 5 V supply and a stable WiFi link. If you're seeing artifacts here, first try a better USB cable or a powered hub before blaming the firmware.
- **HD / SXGA / UXGA (12–14)**: works only on M5 Unit CamS3 (8 MB OPI PSRAM) in practice. AI-Thinker's 4 MB QSPI PSRAM is too slow to sustain >1 MP streaming.

### Persistence & factory-reset

Camera settings live in NVS namespace `craw_camera` (keys `fs`, `q`, `vf`, `hm`) — completely separate from the `craw_config` namespace used by `craw_nvs` for WiFi credentials. You can blow away camera tuning without re-provisioning:

- **`cam-reset-settings`** at the REPL → clears camera NVS only
- **`prov-reset`** at the REPL → clears WiFi creds only (reboots)
- **Full `esptool.py erase_flash`** → both, plus everything else

Same `/control` HTTP endpoints (`/control?var=framesize&val=8`, `/control?var=quality&val=10`, `/control?var=vflip&val=1`, etc.) also persist — anything you tune from the web UI sticks across reboots.

## HTTP endpoints (stock CameraWebServer compatible)

| Endpoint | Method | Response |
|---|---|---|
| `/` | GET | Minimal HTML with `<img src="/stream">` + links |
| `/stream` | GET | `multipart/x-mixed-replace;boundary=…` MJPEG |
| `/capture` | GET | Single `image/jpeg` |
| `/control?var=NAME&val=N` | GET | `{"ok":true}` — vars: `framesize`, `quality`, `brightness`, `contrast`, `saturation`, `hmirror`, `vflip`, `flash` |
| `/status` | GET | JSON: board, framesize, quality, brightness, contrast, saturation, hmirror, vflip, flash_gpio |

Wire format for `/stream` is identical to the Arduino CameraWebServer, so anything that consumed that (browsers, OpenCV `VideoCapture`, Home Assistant `generic_mjpeg`, Frigate, VLC, ffmpeg) works unchanged.

## mDNS

The node publishes two records:

- **Hostname**: `magnet-cam-<MAC4>.local` → A-record → IP
- **Services**:
  - `_http._tcp` on port 80 — so any Bonjour browser finds the stream
  - `_magnet-node._tcp` port 0 with TXT `role=spy caps=camera,jpeg ver=1` — for hive-aware peers

`http://magnet-cam-a1b2.local/stream` works from any client with mDNS (macOS, iOS, most Linux distros, Windows 10+). Fallback if mDNS is blocked on your network: the `ip_address` BLE characteristic still works, and the ruler's peer table still shows the IP.

## Known limitations / future work

- **SD card not supported** on AI-Thinker in this project: the board's SD pins share signals with camera data lines. Would require giving up camera to use SD. M5CamS3 may or may not have an SD slot depending on unit revision.
- **No on-device AI inference yet**. The future path is Milestone-C role bundles: the ruler sends a signed Forth bundle carrying an ESP-DL inference graph + glue words, and the camera node evaluates it per frame. Not in scope for v0.1.
- **`/control` parameter coverage is a subset** of the Arduino CameraWebServer: we expose framesize, quality, brightness, contrast, saturation, hmirror, vflip, flash. Special effects (sepia, AEC/AGC/AWB modes, exposure / gain ceiling) are not wired yet — easy to add by extending `http_stream.c` `control_handler()`.
- **MJPEG under WiFi load**: pulling `/stream` saturates the WiFi uplink, which can cause hive PINGs to timeout. Acceptable for single-stream viewing; for multi-viewer deployments, consider running a local MJPEG relay and reducing frame size.
- **AI-Thinker color-flash / dropped frames at high resolution**: the AI-Thinker board's 3.3 V regulator brown-outs during WiFi TX bursts that coincide with frame capture, producing color-flash artifacts or crashes. Lower the framesize (`8 cam-framesize` = VGA, or `5` = QVGA) until stable. Hardware workaround: add a bulk cap (100 µF+) across 3.3V/GND or power via a beefier USB cable/supply. The VGA default is chosen to avoid this on typical setups.
- **`cam_hal: NO-SOI - JPEG start marker missing` (sensor output corruption)**: the sensor is transmitting but the bytes don't start with `0xFFD8`. Usually a clock-phase issue: lower XCLK gives the ESP32's I2S DMA more margin to latch PCLK edges cleanly. Fix: `10 cam-xclk-mhz` (or `8` for a more conservative fallback) at the REPL, then reboot. Pairs well with dropping framesize. If errors persist at 8 MHz, check the 24-pin FFC ribbon cable seating — it's a common physical culprit on AI-Thinker.

## Troubleshooting on macOS

If the camera is clearly online (serial shows IP, hive ruler sees it join) but you can't reach `http://magnet-cam-<mac4>.local/` or `.local` resolution fails from your Mac, it's almost certainly a macOS privacy-layer issue (Local Network permission, iCloud Private Relay, Private Wi-Fi MAC, firewall). See **[`../docs/macOS-LAN-networking.md`](../docs/macOS-LAN-networking.md)** for a symptom → cause lookup and step-by-step fix.

## Related projects in this repo

- `../M5Atom_Echo_Hex_Hive_Test/` — reference implementation of the same post-WiFi bringup sequence on classic ESP32 with I2S audio + Unit Hex display
- `../M5StackDial-m5gfx-demo-ESPIDFORTH/` — the hive ruler (advertises `_magnet-ruler._tcp`, accepts this camera as a peer)
- `../scripts/fake_ruler.py` — laptop-side ruler for bringup without the Dial
- `../docs/MagNET-HiveProtocol-v1.md` — on-wire protocol spec
- `../components/craw_camera/` — the shared pin-map + camera-wrapper component; add new boards by dropping a `pins_xxx.h` header and extending the enum
