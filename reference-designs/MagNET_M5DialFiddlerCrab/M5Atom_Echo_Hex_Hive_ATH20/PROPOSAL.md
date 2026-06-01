# M5Atom_Echo_Hex_Hive_ATH20 — Proposal

Wrap up UC2 by replacing simulated environment values with a real
temperature + humidity feed from an Adafruit AHT20, displayed live on
the M5 Atom HEX's RGB LEDs. Forked from the existing
`M5Atom_Echo_Hex_Hive_Test` project; the changes are additive (sensor
read loop + LED gauge paint + HTTP endpoint) and don't disturb the
audio / network plumbing already in `_Test`.

> **Architecture note.** `_Test` is **ESP-IDF + ESPIDFORTH** (`framework
> = espidf`, `idf_component_register`, `craw_nvs`/`craw_wifi`/
> `craw_ble_provision`/`craw_hive`, RMT-based `led_strip`, Forth REPL) —
> same shape as `XIAO_ESP32C3_IOT_LIGHTING` and `MagNET_ReSpeaker_Boombox`.
> This proposal assumes the same toolchain. No Arduino libraries.

---

## 1. Sensor choice (settled by the part)

The Adafruit AHT20 ([learn.adafruit.com/adafruit-aht20](https://learn.adafruit.com/adafruit-aht20))
measures **temperature + relative humidity only** — barometric pressure
is a BMP/BME280 feature and isn't on this board. So "keep or drop
barometric pressure" is moot; the AHT20 already drops it. If pressure
is wanted later, swap in a BME280 (same I²C bus, drop-in firmware
change). For UC2 the env-hub manifest entry stays "simulated" for
barometer + pollen until/unless a BME680-class board lands.

Board characteristics relevant to wiring:
- VIN tolerates 3.0–5.5 V (on-board regulator + 5 V-tolerant I²C shifters)
- I²C address `0x38` (fixed; no jumper)
- Two STEMMA QT (JST SH 4-pin, 3.3 V) ports, unused in this proposal
- Through-hole header for VIN / GND / SDA / SCL
- AHT20 has internal pull-ups on SDA/SCL — no external pull-ups needed

### 1.5 One AHT20 per bus

Address `0x38` is fixed in silicon. Two AHT20s on the same I²C bus is
impossible without an I²C multiplexer (e.g. TCA9548A). Not a problem
for this device, but worth noting before someone tries to chain two.

---

## 2. Recommended connection — Atom Echo bottom-pad I²C (NOT the HEX Grove)

**Critical correction from an earlier draft of this proposal:** the HEX
Grove port cannot host I²C. `_Test` drives the HEX's 37-LED chain on
**GPIO 26** (`HEX_LED_GPIO = 26`), which is the Atom Echo's Grove yellow
pin. Wiring a sensor's SDA onto that same pin produces garbage on both
— WS2812 data and I²C SDA can't share a wire. The HEX Grove pass-through
only frees GPIO 32 downstream, which is one wire short of an I²C bus.

The sensor wires to the **Atom Echo's bottom-side solder pads** instead.
`_Test`'s current GPIO use leaves G21 and G25 free:

| Used by `_Test` | Pin | Function |
|---|---|---|
| ✓ | G19 | I²S BCLK (speaker) |
| ✓ | G22 | I²S DOUT (speaker) |
| ✓ | G26 | HEX LED data (G26 = Grove yellow) |
| ✓ | G33 | I²S WS (speaker) |
| ✓ | G39 | onboard button (input-only) |
| · | G21 | **AHT20 SDA** (this proposal) |
| · | G25 | **AHT20 SCL** (this proposal) |
| · | G23 | free |

```
M5 Atom Echo bottom pads             Adafruit AHT20 breakout
┌─────────────────┐                  ┌──────────────────┐
│  5V             │──────────────────┤ VIN  (3–5.5 V)   │
│  GND            │──────────────────┤ GND              │
│  G21            │───── SDA ────────┤ SDA              │
│  G25            │───── SCL ────────┤ SCL              │
└─────────────────┘                  └──────────────────┘
       (no soldering on Atom required if you use clip-on
        DuPont leads to the through-hole pads;
        STEMMA QT ports on the breakout unused)
```

**Verify before you order parts.** Probe the Atom Echo's bottom pads
with a multimeter / continuity tester to confirm G21 and G25 are the
labels you expect on your unit. M5's silk-screens have rotated across
hardware revisions.

Pull-ups: the AHT20 board has internal pull-ups. The Atom Echo's bottom
pads do not. No external pull-ups required.

---

## 3. LED display — left/right hex gauge

The M5 Atom HEX's RGB LEDs (37× SK6812 on G26) are split down the
vertical centerline:

```
       Left half (temperature)         Right half (humidity)
                ▲                              ▲
              cool → hot                     dry → wet
        blue → magenta → red           dim-cyan → bright-cyan
              0 °C → 40 °C                 0 % → 100 %

           ●   ●                        ●   ●
         ●   ●   ●                    ●   ●   ●
       ●   ●   ●   ●                ●   ●   ●   ●
         ●   ●   ●                    ●   ●   ●
           ●   ●                        ●   ●
```

Two complementary encodings so the reading is glanceable under varied
lighting:

- **Hue** carries the value (left half = blue→red thermal gradient;
  right half = cyan saturation ramp).
- **Fill height** — how many LEDs in that half are lit, bottom row up
  — gives a tactile "bar" read even if hue is hard to read in bright
  sunlight.

The center-column LEDs are reserved for **status** (see §6 / UDM §10.9).
The gauge owns the left/right halves only.

LED index → gauge-cell mapping lives in `docs/led_mapping.md` (filled in
during implementation once the actual chain order is confirmed on the
bench with the existing `hex-test` Forth word from `_Test`).

---

## 4. Firmware plan (ESP-IDF + ESPIDFORTH)

Project layout — same shape as `_Test` and the lighting / boombox nodes:

```
M5Atom_Echo_Hex_Hive_ATH20/
├── platformio.ini                # framework=espidf, board=m5stack-atom
├── partitions.csv                # inherit from _Test
├── sdkconfig.defaults            # inherit + LWIP_MAX_SOCKETS=16 +
│                                 #   HTTPD_MAX_REQ_HDR_LEN=1024
├── components/                   # forth + craw_* (copied from _Test)
├── src/
│   ├── CMakeLists.txt            # add esp_http_server, mdns, json,
│   │                             #   esp_driver_i2c to REQUIRES
│   ├── idf_component.yml         # add espressif/mdns
│   ├── main.c                    # forked from _Test; adds AHT20 init,
│   │                             #   sensor task, gauge paint, HTTP API
│   ├── aht20.h / aht20.c         # I²C driver (raw transactions —
│   │                             #   no Adafruit_AHTX0)
│   └── http_env.c / http_env.h   # HTTP API (mirrors http_strip.c)
├── docs/
│   ├── wiring.md                 # photo + the bottom-pad diagram from §2
│   └── led_mapping.md            # LED index → gauge cell
├── tests/
│   └── http-smoke.sh             # GET environment / temperature / humidity
├── PROPOSAL.md                   # this file
└── README.md
```

**No Arduino libraries.** The AHT20 protocol is trivial in raw I²C:

```
0xAC 0x33 0x00     ← trigger measurement
wait ~80 ms
read 7 bytes       → [status, hum[19..12], hum[11..4],
                      hum[3..0]|temp[19..16], temp[15..8],
                      temp[7..0], crc]
RH%  = hum_raw / 2^20 * 100
T °C = temp_raw / 2^20 * 200 - 50
```

`src/aht20.c` is ~80 lines: bus init via `i2c_new_master_bus` /
`i2c_master_bus_add_device` (ESP-IDF v5 driver), one calibration cmd
(`0xBE 0x08 0x00`) at boot, a `aht20_read(float *t_c, float *rh_pct)`
that the sensor task calls every 2 s.

**Loop shape:**

```
every 2 s:
  aht20_read(&t_c, &rh_pct)
  apply calibration offsets (NVS-backed, see §5)
  paint_left_half(temperature_to_gauge(t_c))
  paint_right_half(humidity_to_gauge(rh_pct))
  led_strip_refresh()
  publish to hive KV (existing _Test path) AND
  cache for the HTTP GET handlers (mutex-protected)
```

**LED gauge** uses `led_strip` RMT (already in `_Test`'s `hex_*`
helpers) — no FastLED. The split-half painter just calls
`led_strip_set_pixel()` for indices belonging to each half.

**Forth FFI words** (registered alongside `_Test`'s):
- `temp?` — push current temp in centi-Celsius
- `hum?` — push current humidity in centi-percent
- `env` — print current temp/hum + status
- `cal-temp` ( centi-c -- ) / `cal-hum` ( centi-pct -- ) — calibration
  offsets, persisted to NVS (boombox-vol pattern)

**HTTP API** (mirrors `http_strip.c` / `http_speaker.c`):
- `GET /api/v1/sensor/environment` →
  `{ temperature_c, humidity_pct, ts_ms, hostname, ... }`
- `GET /api/v1/sensor/temperature` → `{ value_c, ts_ms }`
- `GET /api/v1/sensor/humidity`    → `{ value_pct, ts_ms }`
- CORS + wildcard OPTIONS preflight (same pattern).
- `http_env_start()` fires from `on_wifi_event(CONNECTED)`.

`prov_cfg.appearance` should be set to a temp/humidity-sensor category
once we pick a value from the BT SIG Assigned Numbers PDF (defer; not
blocking). `0x00 + 0x015` (Sensors) family is the lookup target —
specific subcategories like "Temperature Sensor" land somewhere in the
0x05C0–0x05FF range. **TODO.**

Code reuse: BLE/WiFi provisioning, hive join, audio, the `hex-test`
Forth word, button handling — all come from `_Test` unchanged.

---

## 5. Calibration note (suggested config knob)

The AHT20, mounted close to a warm ESP32 enclosure, typically reads
~1–2 °C above ambient. Worth exposing two config values in NVS:

- `TEMP_OFFSET_C` (default `-1.5`)
- `HUMIDITY_OFFSET_PCT` (default `0`)

Plumbed via NVS the same way the boombox stores its volume
(`boombox_nvs_load` / `boombox_nvs_save_vol` in
`MagNET_ReSpeaker_Boombox/src/main.c`) so the pattern's consistent
across the family. Forth words `cal-temp` / `cal-hum` set them at the
REPL; offsets apply between raw read and gauge paint / HTTP response.

---

## 6. Open questions

### OQ-1 — mDNS / Bonjour autoconfiguration *(user-flagged)*

Every family node already publishes mDNS via `magnet-<role>-<MAC4>.local`
(lighting, boombox, vitals). The proactive-discovery half — a
server-side mDNS *browser* that subscribes to `_http._tcp.local` and
maintains a live device table — is the actual gap. Worth a half-day
spike:

- **Server side.** A small mDNS browser (e.g. `zeroconf` in Python, or
  `bonjour-service` in Node) subscribes and maintains a device table.
  TXT keys would standardize on `role`, `id`, `fw`, `loc`.
- **Trade-off.** Plug-and-play hardware vs. a new failure mode if
  multicast is flaky on the deployment Wi-Fi (some enterprise / guest
  networks block multicast outright; macOS Local Network permission is
  its own gotcha — see [[project_macos_lan_gotchas]]).
- **Outcome bar.** If multicast works end-to-end on the target Wi-Fi,
  roll out fleet-wide; if not, fall back to today's static `*_HOST` env
  vars and revisit when deployment networks change.

### OQ-2 — boot / fault indicator policy

**Partially settled.** `specs/UDM-MagNET-v1.md` §10 entry 9 already
declares the standard hive-node provisioning UX vocabulary:

| State | Visual |
|---|---|
| Unconfigured (no creds, advertising) | orange flash ~1 Hz |
| Connecting (creds received → connecting) | orange flash ~3 Hz |
| Connected | green breathing ~0.4 Hz |
| Failed | red strobe ~8 Hz |
| BLE characteristic write (transient) | white pulse ~250 ms |

**This proposal adopts that vocabulary on the hex's center column**
(the LEDs reserved between the left/right gauge halves). Once WiFi is
up the gauge takes the left + right halves; the center stays as a
status indicator (or pulses softly green-breathing as a "live" tell).
That keeps a single visual language across single-LED nodes
(`craw_status_led`) and panel-equipped nodes (lighting strip, hex
gauge).

Still to settle in this proposal: OTA-in-progress (slow rainbow wipe?)
and sensor-read-failed (alternating-blink on the affected gauge half?).
Lean toward "alternating red on the affected half" for sensor failure
so the surviving half's gauge stays readable.

### OQ-3 — OTA update path

Out of scope for this project. Naturally pairs with OQ-1 (discovered
devices = firmware-update target set). Revisit after UC2 ships.

---

## 7. What else is left on UC2?

Grounded against the family's actual shipped state (2026-05-19):

**Already real (wired through Vite proxy):**
- LED strip → `XIAO_ESP32C3_IOT_LIGHTING` (Grove WS2813) ✓
- Speaker → `MagNET_ReSpeaker_Boombox` (`craw_audio` chime/doorbell) ✓

**Mock today — this proposal addresses one:**
- 🔄 Temp + humidity → this project. UC2's manifest already has a
  `usm_service_endpoint` pointing at `magnet-stamp-c3u-a1b2.local/temperature`;
  either repoint to the Atom Echo's hostname (one JSON edit in
  `examples/uc2-room.json`) or have the new firmware publish under
  that hostname.

**Mock; defer (accept as "permanently mocked for the UC2 deliverable"
unless real hardware is on hand):**
- Light bulb (`/api/v1/actuator/light`) — needs a target device.
- Thermostat (`/api/v1/actuator/thermostat`) — needs a real HVAC
  controller; mocked is reasonable for the demo.
- AQI / barometric pressure / pollen — the env-hub manifest entry
  marks these as simulated; swap when a BME680 + AQI board lands.

**Cross-cutting:**
- mDNS autoconfig spike (OQ-1) — discovery half is the gap.
- Boot/fault indicator vocabulary (OQ-2) — already standardized in
  UDM §10.9; this proposal adopts it.

So the answer to "what else is left on UC2?" is **this proposal** plus
a decision on whether to keep the bulb + thermostat as mock for the
UC2 deliverable. My read: ship UC2 with temp/humidity real and the
remaining two mock; bulb/thermostat become UC2.1 candidates.

---

## 8. Next steps

1. **Probe the Atom Echo bottom pads** with a multimeter to confirm
   G21 / G25 labels match your unit. Document in `docs/wiring.md` with
   a photo.
2. **Scaffold the project** by copying `_Test`'s structure
   (`platformio.ini`, `CMakeLists.txt`, `partitions.csv`, `sdkconfig.*`,
   `components/`, `src/main.c`) into this directory — same way
   `XIAO_ESP32C3_IOT_LIGHTING` forked from `M5Stamp3CU_Blinky_E4TH`.
3. **Build the AHT20 I²C driver** (`src/aht20.{h,c}`). Verify by
   logging temp/hum over USB-JTAG before touching the LEDs.
4. **Run `hex-test`** (Forth word inherited from `_Test`) to confirm
   the actual LED chain order; fill in `docs/led_mapping.md` and the
   `HEX_LEFT_IDX[]` / `HEX_RIGHT_IDX[]` / `HEX_CENTER_IDX[]` tables.
5. **Wire the gauge painter** and the status-overlay on the center
   column (adopt UDM §10.9 vocabulary).
6. **Add the HTTP API** (`src/http_env.{h,c}`) and a `tests/http-smoke.sh`.
   Vite proxy entry `/api/v1/sensor/environment` → `ATH20_HOST` in
   `webxrofthings/prototype/d3-spatial/vite.config.ts`.
7. **Decide on OQ-1 spike** — yes / no / later.
