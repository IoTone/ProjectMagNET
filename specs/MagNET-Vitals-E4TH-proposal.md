# MagNET Vitals E4TH — Personal Health Monitoring Dataspace (UC3)

**Status:** proposal (design only, no code yet)
**Date:** 2026-05-03
**Author:** ProjectMagNET maintainers
**Hardware target:** [Seeed MR60BHA2 mmWave Heart-Rate / Breathing Sensor Kit](https://wiki.seeedstudio.com/getting_started_with_mr60bha2_mmwave_kit/) — XIAO ESP32-C6 + 60 GHz radar + BH1750 lux + WS2812 LED.
**Firmware family:** ESPIDFORTH (Forth REPL on ESP-IDF). New project: `MagNET_Vitals_E4TH`.
**Companion dataspace:** UC3 `kords-personal-health`, served to d3-spatial.
**Spec dependencies:** [UDM-MagNET v1.0](./UDM-MagNET-v1.md), [USM-MagNET v1.0](./UDM-MagNET-v1.md#4-usm-magnet-core).

---

## 1. Why this matters

ProjectMagNET's reference designs to date have been room-scale (UC2 living-room, with camera + temperature). This adds the **personal scale** — a small mmWave radar that surfaces multi-person presence and resting biometrics into a private spatial visualization. Three reasons it's worth building:

1. **Validates UDM/USM v1.0 against a real, multi-service device.** The MR60BHA2 kit naturally exposes 5+ distinct services (heart rate, breathing, presence, multi-target tracking, light level, the LED actuator). Modeling them tests every mechanism we just shipped: `usm_polling_x`, characteristic constraints, deviceRef/serviceRef linkage, multi-service-per-device.
2. **Demonstrates a different *scale* of dataspace.** UC2 is room-scale; UC3 is `personal` scale. Different default placement radius, different privacy posture, different mark choices.
3. **Forces us to confront the health-data privacy story** before it sneaks up later. Even at low fidelity, biometrics are intimate — getting the "who can join, what they see, what gets stored" model right here pays off across every future health/wellness scenario.

### 1.1 Demo framing — "restful vitals," not sleep tracking

Seeed's spec is clear that HR/BR accuracy is best in sleep posture (1.5 m, 45° tilt, low motion). **We can't and won't film sleep for a demo.** The framing is therefore:

- **Primary signal**: presence + multi-target tracking + breathing rate. These are robust across postures.
- **Secondary signal**: heart rate **with confidence** — every HR reading is annotated with a `signal_q` 0–100 score. The renderer dims the HR mark proportionally; low-confidence readings appear as ghosted lines, not authoritative numbers.
- **Demo posture**: subject seated still or briefly reclined; the dataspace explicitly admits this is non-clinical, illustrative.
- **The thing the demo actually shows**: how a hyperlocal IoT *system* surfaces ambient biometrics responsibly, not how accurate this particular sensor is.

The `udm_tags: ["sleep-only"]` flag stays on the device record (it's a faithful description of the device's accuracy envelope) and the `show-privacy` HUD action surfaces it. We're transparent about the constraint, not pretending it doesn't exist.

Non-goals for this proposal:
- This is **not** a medical device. Anything we build is observational telemetry, not clinical.
- No HRV. The MR60BHA2 datasheet does not expose beat-to-beat intervals. If HRV becomes important, supplement with a chest strap or PPG.
- No sleep-staging, sleep-scoring, or any narrative wrapping nightly data into "you slept X hours." Out of scope and out of accuracy.

---

## 2. Hardware

The MR60BHA2 mmWave Sensor Kit ships as a single board carrying:

| Part | Role |
|---|---|
| MR60BHA2 60 GHz radar module | HR + BR + presence + distance + phase data; UART out |
| XIAO ESP32-C6 | Host MCU; Wi-Fi 6 + Thread/802.15.4 + BLE |
| WS2812 RGB LED | Status indication (matches the `Blinky_E4TH` pattern) |
| BH1750 light sensor | Ambient lux 1–65 535; I²C |
| Grove GPIO | Expansion |

**Operational envelope** (per Seeed docs):
- HR/BR detection range: ≤ 1.5 m
- Presence range: ≤ 6 m
- **Recommended use: sleep-monitoring scenarios only.** Desk-sitting and exercise will produce significant inaccuracies. This is a fundamental constraint, not a tooling issue.
- Mounting: ~1 m height, 45° tilt toward the chest for sleep use.
- Avoid: nearby moving curtains, water flow, large metal/mirror surfaces, glass-through-detection, low-quality PSUs.
- Power: 5 V / 1 A, ~0.5 W standby / ~0.8 W active.

**Communication:** UART 115 200 baud, "Tiny Frame Interface" protocol. Seeed publishes an Arduino library (`Love4yzp/Seeed-mmWave-library`) with the high-level API: `getHeartRate()`, `getBreathRate()`, `getDistance()`, `getHeartBreathPhases()`, `getPeopleCountingTargetInfo()`.

---

## 3. Firmware project: `MagNET_Vitals_E4TH`

Lives at `reference-designs/MagNET_M5DialFiddlerCrab/MagNET_Vitals_E4TH/` (sibling to the existing `M5Stamp3CU_Blinky_E4TH`). Same project shape, same component reuse where possible.

### 3.1 Component layout

```
MagNET_Vitals_E4TH/
├── platformio.ini           # board = seeed_xiao_esp32c6
├── sdkconfig.defaults       # IDF defaults (mDNS, USB-JTAG-serial, BT enabled)
├── partitions.csv
├── CMakeLists.txt
├── src/
│   ├── main.c               # WS2812 modes + Forth REPL + HTTP server + life-cycle
│   ├── http_vitals.c        # esp_http_server routes (see §6)
│   ├── http_vitals.h
│   └── CMakeLists.txt
└── components/
    ├── forth/               # ESPIDFORTH (copied from existing project)
    ├── craw_mr60bha2/       # NEW — see §4
    ├── craw_bh1750/         # NEW — minimal I²C lux read
    ├── craw_nvs/            # reused from M5Dial project
    ├── craw_wifi/           # reused
    ├── craw_ble_provision/  # reused — same BLE GATT, same UUID layout
    └── craw_hive/           # reused — joins the same hive as other MagNET nodes
```

### 3.2 What is reused vs. new

**Reused** (verbatim or minor):
- `craw_nvs`, `craw_wifi`, `craw_ble_provision` — provisioning over BLE, WiFi creds in NVS, identical to the M5StampC3U pattern. The dial-side ruler discovers this node like any other MagNET hive member.
- `craw_hive` — node joins `MagNET-biologic-<MAC4>` hive, advertises role `vitals`.
- `forth/` — ESPIDFORTH component, used straight.

**New** (~3 files each):
- `craw_mr60bha2` — UART driver + Tiny-Frame parser + ring-buffer history. ESP-IDF native (we re-implement the small portion of the Seeed Arduino library we need; the protocol is documented). Public API mirrors the Seeed lib: `craw_mr60_get_hr(uint16_t* bpm, uint8_t* signal_q)`, `craw_mr60_get_br(uint16_t* rpm, ...)`, etc.
- `craw_bh1750` — 30-line I²C lux read. Trivial.

### 3.3 Boot-time flow

1. USB-serial-JTAG up; print banner.
2. WS2812 init; show **amber** = booting.
3. NVS + WiFi STA + BLE provisioning (same as the M5StampC3U project).
4. UART2 init at 115 200 baud → `craw_mr60bha2_start()`. Spawn a task that reads frames continuously, decodes them, and updates a shared `vitals_state_t` protected by a mutex. Append to a 60-sample ring buffer at 1 sample/min for HR and BR.
5. `craw_bh1750_init()` over I²C; read once per second.
6. `forth_init()` + `register_vitals_words()` (see §5).
7. `http_vitals_start()` (see §6).
8. WS2812 → **soft cyan** = idle / no presence; **gentle pulse, hue tied to current HR** when a person is detected.
9. `forth_repl()` blocks forever.

LED hue mapping from HR — leans into the device's nature without inventing data: `hue = remap(bpm, 50, 100, 220° blue-violet, 0° red)`. This is a visual signal, not a clinical chart; the spatial-d3 view is where actual data lives.

---

## 4. `craw_mr60bha2` UART driver

Public header (`craw_mr60bha2.h`):

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint16_t bpm;            // beats per minute
    uint16_t rpm;            // breaths per minute
    uint8_t  signal_q_hr;    // 0..100
    uint8_t  signal_q_br;    // 0..100
    bool     presence;       // someone in detection cone
    uint16_t distance_cm;    // 0 if no presence
    int64_t  updated_us;     // esp_timer_get_time() at last frame
} craw_mr60_state_t;

esp_err_t craw_mr60_init(uart_port_t port, int rx_gpio, int tx_gpio);
esp_err_t craw_mr60_get_state(craw_mr60_state_t* out);
esp_err_t craw_mr60_get_hr_history(uint64_t* t_ms, uint16_t* bpm, size_t cap, size_t* out_count);
esp_err_t craw_mr60_get_br_history(uint64_t* t_ms, uint16_t* rpm, size_t cap, size_t* out_count);
```

Background task:
- Reads 256-byte chunks from UART
- Parses Tiny-Frame frames (header + length + type + payload + CRC)
- Per known frame type: writes into the shared state under a portMUX
- On a 60 s cadence, snapshot HR and BR into ring buffers (60 entries each)

Buffer math: 2 × 60 entries × (8 + 2 + 2) ≈ 1.5 KB. Trivial on C6.

### 4.1 Frame-type discovery

Seeed's Tiny-Frame protocol identifies HR / BR / phase / presence / multi-target frames by `type`. We mirror the Arduino library's enum exactly so future Seeed updates are easy to track. Initial set we plan to consume:

- `HEARTBEAT` (HR bpm + signal quality)
- `BREATHRATE` (RPM + signal quality)
- `PRESENCE`
- `DISTANCE`
- `HEART_BREATH_PHASES` (optional; advanced viz)
- `MULTI_TARGET_INFO` (optional; for the multi-person tag in §10)

Anything else, log + discard.

---

## 5. Forth vocabulary — `register_vitals_words()`

Naming follows the existing `Blinky_E4TH` conventions: `?` suffix for queries; bare verb for actions; print-style words mirror `prov-status`.

| Word | Stack | Description |
|---|---|---|
| `hr?` | ( -- bpm ) | Push current heart rate (0 if no presence). |
| `hr-q?` | ( -- 0..100 ) | Push HR signal-quality score. |
| `br?` | ( -- rpm ) | Push current breathing rate. |
| `br-q?` | ( -- 0..100 ) | Push BR signal-quality score. |
| `presence?` | ( -- 0\|1 ) | Push 1 if a person is in the cone. |
| `distance?` | ( -- cm ) | Distance to detected target in cm. |
| `lux?` | ( -- lux ) | Push BH1750 lux reading. |
| `vitals` | ( -- ) | Print snapshot — HR, BR, presence, lux, signal quality. |
| `hr-history.` | ( -- ) | Print last 60 HR samples (one per line, ring-buffer order). |
| `br-history.` | ( -- ) | Same for BR. |
| `mr60-status` | ( -- ) | Print radar firmware version + last frame type / age. |
| `mr60-reset` | ( -- ) | Soft-reset the radar over UART. |
| `led-vitals` | ( on/off -- ) | Enable/disable the HR-hue LED indicator. |

Composition example at the REPL:

```
ok> presence? if hr? . ." bpm" else ." nobody home" then
72 bpm
ok>
```

This is the value of having Forth on the device — quick interactive probing of the sensor without rebuilding firmware.

---

## 6. HTTP services

Single `esp_http_server` instance on port 80. All endpoints return JSON, all set `Access-Control-Allow-Origin: *` (LAN-only by tag, but CORS unlocks browser fetch from `d3-spatial` running on a tunnel URL). Mirror the UC2 temperature pattern.

| Method + path | Body | Notes |
|---|---|---|
| `GET /vitals` | `{ bpm, rpm, presence, distance_cm, lux, signal_q_hr, signal_q_br, timestamp }` | Combined snapshot. |
| `GET /heart-rate` | `{ bpm, signal_q, presence, timestamp }` | Single value. |
| `GET /heart-rate/history` | `{ samples: [{t: ms, v: bpm}, ...] }` | Last 60 minutes. |
| `GET /breathing` | `{ rpm, signal_q, timestamp }` | Single value. |
| `GET /breathing/history` | `{ samples: [{t, v}] }` | Last 60 minutes. |
| `GET /presence` | `{ present, since_ms }` | Boolean + how long. |
| `GET /lux` | `{ lux, timestamp }` | Ambient light. |
| `OPTIONS *` | 204 | CORS preflight. |

History endpoints match the `series` shape the d3-spatial `line` mark already consumes (`{ t: ms-since-epoch, v: number }`), and the `distributions` shape the `streamgraph` mark consumes (when we emit per-zone-minute counts — see §10.2).

---

## 7. UDM device record (UDM-MagNET v1.0)

```jsonc
{
  "udm_key":   "magnet-vitals-c0d4",
  "udm_uuid":  "urn:device:xiao-esp32c6-mr60bha2:c0d4",
  "_comment":  "Replace c0d4 with the real MAC suffix at flash time.",
  "udm_model_name":   "MR60BHA2 mmWave Heart-Rate Kit",
  "udm_model_number": "SKU 110992151 (Seeed)",
  "udm_vendor":       "Seeed Studio",
  "udm_mktg_name":    "Magnet Vitals",
  "udm_type":         "health-sensor",
  "udm_class":        ["health-sensor", "radar", "mmwave", "wifi", "ble"],
  "udm_capabilities": [
    "heart-rate", "breathing-rate", "presence-detection",
    "ambient-light", "json-http", "forth-repl",
    "non-contact", "sleep-only"
  ],
  "udm_chipset_details": {
    "vendor": "Espressif", "type": "ESP32-C6",
    "frequency": "160MHz", "core_count": 1,
    "instruction_set": "rv32imac"
  },
  "udm_memory_volatile":     { "size": "512KB", "type": "SRAM" },
  "udm_memory_non_volatile": { "size": "4MB",   "type": "flash" },
  "udm_sensors": [
    "seeed-mr60bha2-mmwave",
    "rohm-bh1750-lux"
  ],
  "udm_services_link": [
    "vitals-current", "hr-current", "hr-history",
    "br-current", "br-history", "presence", "lux"
  ],
  "udm_tags": [
    "personal", "lan-only", "no-tls",
    "health-data", "sleep-only", "non-contact"
  ],
  "udm_spatial_anchor_x": { "x": 0.0, "y": 1.0, "z": -0.4 },
  "udm_fw_version_x": "vitals-e4th-0.1.0"
}
```

Three things worth flagging:

- `udm_type: "health-sensor"` is a new value not yet enumerated as conventional in v1.0 (the v1.0 spec lists `camera`, `compute-node`, `sensor`, `actuator`, `display`, `gateway`, `wearable`). I suggest **adding `health-sensor` to the conventional set in UDM-MagNET v1.1** alongside `wearable` — it's distinct enough (passive, observed-from-environment, biometric) to justify its own bucket.
- `sleep-only` in `udm_tags` is a deployment-context flag honoring Seeed's recommendation. Tooling may surface a banner when this tag is present + the dataspace shows daytime data ("readings during this window are likely inaccurate per the device's spec").
- `udm_spatial_anchor_x` puts the radar at floor-side-of-bed height; the d3-spatial renderer can place a device pin glyph here.

---

## 8. USM service records (USM-MagNET v1.0)

Six services. Listed compactly; only the most data-rich shown in full.

```jsonc
"usm_services": [
  {
    "usm_key": "vitals-current",
    "usm_uuid": "urn:service:vitals:c0d4",
    "usm_service_name": "Vitals snapshot",
    "usm_type": "sensor",
    "usm_class": ["sensor", "biometric", "snapshot"],
    "usm_service_endpoint": {
      "GET": { "url": "http://magnet-vitals-c0d4.local/vitals", "content-type": "application/json" }
    },
    "usm_polling_x": { "interval_seconds": 2 },
    "characteristics": [
      { "usm_characteristic_id": "bpm",  "usm_characteristic_format": "integer", "usm_characteristic_constraints": { "min": 30, "max": 220, "unit": "beats/min" } },
      { "usm_characteristic_id": "rpm",  "usm_characteristic_format": "integer", "usm_characteristic_constraints": { "min": 4,  "max": 60,  "unit": "breaths/min" } },
      { "usm_characteristic_id": "presence",  "usm_characteristic_format": "boolean" },
      { "usm_characteristic_id": "distance_cm", "usm_characteristic_format": "integer", "usm_characteristic_constraints": { "min": 0, "max": 600 } },
      { "usm_characteristic_id": "lux",  "usm_characteristic_format": "number", "usm_characteristic_constraints": { "min": 0, "max": 65535 } },
      { "usm_characteristic_id": "signal_q_hr", "usm_characteristic_format": "integer", "usm_characteristic_constraints": { "min": 0, "max": 100 } },
      { "usm_characteristic_id": "signal_q_br", "usm_characteristic_format": "integer", "usm_characteristic_constraints": { "min": 0, "max": 100 } },
      { "usm_characteristic_id": "timestamp", "usm_characteristic_format": "iso8601" }
    ],
    "usm_tags": ["live", "lan-only", "biometric"]
  },

  {
    "usm_key": "hr-history",
    "usm_uuid": "urn:service:heart-rate-series:c0d4",
    "usm_service_name": "Heart-rate history (last 60 min, 1 sample/min)",
    "usm_type": "timeseries",
    "usm_class": ["timeseries", "biometric", "heart-rate"],
    "usm_service_endpoint": {
      "GET": { "url": "http://magnet-vitals-c0d4.local/heart-rate/history", "content-type": "application/json" }
    },
    "usm_polling_x": { "interval_seconds": 30 },
    "characteristics": [
      { "usm_characteristic_id": "samples", "usm_characteristic_format": "array<{t,v}>",
        "usm_characteristic_constraints": { "v_unit": "beats/min", "v_min": 0, "v_max": 220, "max_length": 60, "t_unit": "ms_since_epoch" } }
    ],
    "usm_tags": ["lan-only", "biometric"]
  },

  // hr-current, br-current, br-history, presence, lux follow the same pattern (elided).
]
```

Three things UC3 stresses about USM-MagNET v1.0:

1. **`usm_polling_x` is heavily used** — every service needs a polling cadence; this confirms it should be promoted to a normative (non-`_x`) field in v1.1.
2. **`usm_characteristic_constraints` carries `unit`, `v_unit`, `t_unit`** — these aren't explicitly in v1.0's vocabulary. They're free-form per-service today. Worth proposing a small **standard set of unit strings** (`beats/min`, `breaths/min`, `°C`, `lux`, `cm`, `ms_since_epoch`) so renderers can format axes without parsing per-service.
3. **`array<{t,v}>` works** for time series but is informal. v1.1 could promote to a structured `usm_timeseries_x: { t_field, v_field, t_unit, v_unit }` descriptor.

---

## 9. Dataspace manifest — UC3 `kords-personal-health`

```jsonc
{
  "version": "1",
  "name": "kords-personal-health",
  "scaleTag": "personal",
  "owner": "dkords@gmail.com",
  "acousticEnvironment": "indoor",
  "udm_version": "1.0",
  "udm_doc_version": "0.1.0",

  "_comment_privacy": "Health-data dataspace. Default join model: solo. Sharing requires explicit consent step at join time. See specs/MagNET-Vitals-E4TH-proposal.md §11.",

  "udm_devices": [ /* the magnet-vitals-c0d4 record from §7 */ ],
  "usm_services": [ /* six service records from §8 */ ],

  "marks": [
    {
      "id": "hr-radial",
      "type": "arc",
      "title": "Heart rate",
      "subtitle": "current · ESP32-C6 mmWave",
      "data": { "source": "url", "url": "/api/v1/vitals/current", "shape": "series", "refreshInterval": 2 },
      "config": { "form": "radial-gauge", "min": 30, "max": 180, "warn_below": 50, "warn_above": 110 },
      "deviceRef": "magnet-vitals-c0d4",
      "serviceRef": "vitals-current",
      "hoverable": true
    },
    {
      "id": "hr-line",
      "type": "line",
      "title": "Heart rate · last 60 min",
      "subtitle": "1 sample/min",
      "data": { "source": "url", "url": "/api/v1/vitals/hr-history", "shape": "series", "refreshInterval": 30 },
      "deviceRef": "magnet-vitals-c0d4",
      "serviceRef": "hr-history",
      "hoverable": true
    },
    {
      "id": "br-line",
      "type": "line",
      "title": "Breathing · last 60 min",
      "subtitle": "1 sample/min",
      "data": { "source": "url", "url": "/api/v1/vitals/br-history", "shape": "series", "refreshInterval": 30 },
      "deviceRef": "magnet-vitals-c0d4",
      "serviceRef": "br-history",
      "hoverable": true
    },
    {
      "id": "phases-stream",
      "type": "streamgraph",
      "title": "Heart & breath phases",
      "subtitle": "raw radar phase ribbons · 60s window",
      "data": { "source": "url", "url": "/api/v1/vitals/phases", "shape": "distributions", "refreshInterval": 1 },
      "config": {
        "categories": ["heart_phase", "breath_phase", "total_phase"],
        "windowSize": 120,
        "scrollSpeed": 12
      },
      "deviceRef": "magnet-vitals-c0d4",
      "serviceRef": "phases",
      "hoverable": true
    },
    {
      "id": "targets-map",
      "type": "scatter",
      "title": "Detected targets",
      "subtitle": "multi-target floor map · radar cone",
      "data": { "source": "url", "url": "/api/v1/vitals/targets", "shape": "series", "refreshInterval": 1 },
      "config": { "form": "floor-plan", "extent_m": 6, "show_cone": true, "show_id": true },
      "deviceRef": "magnet-vitals-c0d4",
      "serviceRef": "targets",
      "hoverable": true
    },
    {
      "id": "lux-line",
      "type": "line",
      "title": "Ambient light",
      "subtitle": "lux · BH1750",
      "data": { "source": "url", "url": "/api/v1/vitals/lux-history", "shape": "series", "refreshInterval": 30 },
      "deviceRef": "magnet-vitals-c0d4",
      "serviceRef": "lux",
      "hoverable": true
    }
  ],

  "hud": {
    "items": [
      { "id": "refresh", "label": "Refresh", "icon": "🔄", "action": "reload-marks" },
      { "id": "consent-info", "label": "Privacy", "icon": "🔒", "action": "show-privacy" },
      { "id": "leave",   "label": "Leave",   "icon": "🚪", "action": "leave-dataspace" }
    ],
    "position": "bottom"
  }
}
```

Notes:
- The `hr-zones-stream` mark expects the device (or a small server-side aggregator) to emit per-minute zone histograms. Two implementation options: (a) firmware pre-bins HR and exposes `/heart-rate/zones`; (b) the d3-spatial loader bins on the client. (a) is cheaper for the renderer but couples zone thresholds to firmware. (b) is more flexible. Recommend (b) — bin on the client, per-user thresholds.
- `consent-info` is a new HUD action — see §11.

---

## 10. Spatial-d3 visualization design

### 10.1 Layout (personal scale)

`scaleTag: "personal"` should anchor the marks closer to the user than UC2. Suggested arrangement (looking down at the dataspace centred on the user):

```
                       [ HR radial ]            ← head-height, 0.4 m forward
                             ·
         [ targets map ]  [ HR line ]   [ BR line ]
                             ·
                       [ phases streamgraph ]    ← below center
                             ·
                        [ lux line ]             ← lowest, ambient context
```

The HR radial gauge is the **anchor mark** — biggest, head-on, animated with a soft pulse synced to the current bpm; opacity scaled by `signal_q_hr` so low-confidence readings don't lie. Everything else is supporting context.

### 10.2 The streamgraph fits naturally — phase ribbons, not zones

The MR60BHA2 exposes raw heart and breath **phase** waveforms (continuous floats representing the radar's reconstruction of chest-wall motion). Three channels — `heart_phase`, `breath_phase`, `total_phase` — stack beautifully as wiggle-baseline ribbons. The streamgraph mark we landed on 2026-05-01 ingests this as a `distributions` shape with one row per channel; the scrolling animation matches the live waveform.

Why this beats the original "HR zones over the night" idea:

- **Demoable in seconds, not hours.** Zones-over-night need actual sleep data; phase ribbons stream live the moment a subject sits in the cone.
- **Honest about what the radar does.** Phase data is closer to the source signal — we're showing the device's actual perception, not a derived clinical narrative.
- **Looks great.** Continuous waveforms with smooth wiggle baseline scrolling at ~12 samples/sec produces the most visually compelling animated mark in the gallery.

### 10.3 Multi-target floor map (new mark form)

`getPeopleCountingTargetInfo()` returns a list of detected targets, each with `(x, y)` position relative to the radar and per-target HR/BR estimates. We render this as a **floor-plan scatter mark** (a `scatter` mark with `config.form: "floor-plan"`) — a small panel showing the radar cone with one glyph per target.

This is unique to mmWave (camera-based people counting requires identifiable images; this doesn't) and is the most visually obvious "this is real" demo element. Per-target hover surfaces that target's HR/BR/distance.

`floor-plan` is a new config form for the existing `scatter` mark builder; implementation is small (the cone outline plus instanced sphere positions). v1.1 of the renderer can promote it to its own mark type if multi-target use cases multiply.

### 10.3 Animation cues that respect the data

- **Radial HR gauge**: soft pulse synced to bpm — not a literal heartbeat (that would blink at 1 Hz hardware-limited), but a 2-second envelope modulated by bpm.
- **Lines**: gentle scroll, 1-sample-per-minute means animation is mostly seam-free over 60 minutes.
- **Streamgraph**: scroll the window across the night; stop when paused.
- **Presence pin**: discrete on/off, no animation — a small glyph that fades in/out over 1 s.

### 10.4 Auditory layer (optional)

The dataspace has spatial audio support (Omnitone). Consider a very subtle ambient pad whose density tracks signal-quality; off when no presence. **Off by default; opt-in.**

---

## 11. Privacy & ethics

This is the section that determines whether UC3 ships responsibly.

### 11.1 Threat model

The data on this dataspace identifies, in real time:
- That someone is present in a specific room
- Their heart rate and breathing rate
- Their position (distance from the radar)
- The room's ambient light level (often correlates with sleep/wake)

A hostile party with this data can infer sleep schedule, presence patterns, and stress states. Even short-term storage compounds this.

### 11.2 Default posture

- **LAN-only.** `udm_tags` includes `lan-only`; the firmware MUST NOT make outbound connections to anything off-LAN by default.
- **No persistence beyond the 60-minute ring buffer on-device.** Long-term storage is a separate, opt-in service (not in this proposal).
- **Solo by default.** The dataspace's join code, when generated, defaults to `single-user` mode (only the device owner can join). Multi-user join requires a second consent step.
- **Health-data tag in UDM** (`udm_tags: ["health-data"]`) — the d3-spatial renderer should display a privacy banner the first time a user enters a dataspace tagged this way, summarizing what's exposed and to whom.

### 11.3 New HUD action: `show-privacy`

I propose adding `show-privacy` to the dataspace HUD action set in v1.1 of the manifest schema. When the user invokes it, the renderer displays:

- A list of marks and the underlying `serviceRef` for each
- The `udm_tags` of every device — call out `health-data` and `sleep-only` prominently
- Who is currently joined (none, just-me, +N)
- Where data goes (LAN-only / proxied / stored)
- A "leave dataspace" reminder

The d3-spatial side ships a default implementation; dataspaces can override the body with a markdown URL.

### 11.4 Multi-person scenarios — exposed by default

The MR60BHA2 supports multi-target detection and we **expose all detected targets** as the `targets` service. This was a deliberate decision (resolves L4): multi-target tracking is the most visually compelling demo of the mmWave platform and the dataspace owner is the radar's owner — they see what the radar sees.

- Each target is anonymous (an integer `target_id` from the radar; not a named person).
- Per-target characteristics: `target_id`, `x_m`, `y_m`, `distance_cm`, `bpm`, `rpm`, `signal_q`, `first_seen_ms`.
- `target_id` resets when a target leaves the cone for > N seconds (radar-side, not us).
- The `show-privacy` panel **must** call out: "this dataspace tracks all people in the radar cone, not just you. Anyone else in range will appear as anonymous targets with their HR/BR/position visible to whoever joins this dataspace."
- Joining a multi-target dataspace surfaces this in the consent banner as a distinct line item. The default consent state is **acknowledged-and-acceptable for the device owner only**; multi-user joins re-prompt.

### 11.5 Children

The proposal assumes adult, consenting use. **No child-targeted variant** ships from this firmware as-is. If child-tracking is requested later (e.g. infant breathing monitor), it gets its own proposal with its own legal review.

### 11.6 Telehealth / sharing

Two future-work scenarios outside this proposal:

- **One-shot snapshot share**: the device emits a signed bundle of last-N-minutes data the user can deliver to a clinician via any channel.
- **Persistent share**: a second consenting party joins the dataspace and sees the same marks. Auditable, revocable, time-limited.

Neither lands in v0.1.

---

## 12. Limitations and open questions

| # | Topic | Status |
|---|---|---|
| L1 | Accuracy posture | **Resolved** — report-with-confidence. Every HR/BR sample carries `signal_q` 0–100; renderer dims marks proportionally; HUD never claims clinical accuracy. |
| L2 | HRV | Open. Out of scope for v0.1. Roadmap item with a complementary PPG (BLE chest strap or finger PPG) for v0.2. |
| L3 | Mounting calibration | Open. The 1 m / 45° geometry is highly specific. Need either a mounting jig drawing or a calibration Forth word that prompts the user through a posture check. Punt to Phase 1.5. |
| L4 | Multi-target | **Resolved** — exposed as `targets` service; floor-plan scatter mark visualizes them; privacy panel explicitly names the surface area. |
| L5 | Time sync | Open. HR-history `t` values need real-time clock. Same SNTP path the M5StampC3U project uses; copy-paste in Phase 2. |
| L6 | Consent ledger | Open. Where do we record "user consented to multi-user join at T"? `craw_nvs` `consent` namespace. Address in Phase 5. |
| L7 | UART2 pin choice | Open. XIAO C6 has limited free GPIOs after WS2812 + I²C. Confirm with the kit in hand; should be straightforward with the schematic. |
| L8 | Streamgraph data binning | **Resolved by retiring the original idea.** Phase ribbons (§10.2) are emitted directly by the radar — no zoning logic needed firmware-side or client-side. |
| L9 | UDM v1.1 candidates | **Approved** by the user. Three items to land in v1.1: (a) `health-sensor` as a conventional `udm_type`; (b) `usm_polling_x` → `usm_polling`; (c) standard unit-string set in `usm_characteristic_constraints`. Plus the `show-privacy` HUD action. |

---

## 13. Phased roadmap

| Phase | Scope | Effort | Hardware needed |
|---|---|---|---|
| **0 — Bench-up** | Order kit. Boot stock Seeed firmware. Confirm UART output, dump frames. | 1 day + shipping | MR60BHA2 kit |
| **1 — Firmware MVP** | New `MagNET_Vitals_E4TH` project. `craw_mr60bha2` driver. Forth words `hr?`, `br?`, `presence?`, `vitals`. WiFi + BLE provisioning reused. WS2812 boot/idle/HR-hue indicator. | 1 week | kit |
| **2 — HTTP services** | `esp_http_server`. All endpoints in §6. CORS. Hostname `magnet-vitals-<MAC4>.local`. | 2–3 days | kit |
| **3 — UDM/USM record** | Author the device JSON (§7) + service records (§8). Validate via the `validateManifest` validator. Land a concrete `examples/uc3-personal-health.json` in d3-spatial. | 1 day | none |
| **4 — Dataspace + marks** | Wire `uc3-personal-health.json` end-to-end. Verify all 6 marks render in the gallery shell. Add the `consent-info` HUD action stub. | 2 days | none |
| **5 — Privacy banner** | First-render banner on `health-data`-tagged dataspaces. `show-privacy` HUD action implementation. | 2 days | none |
| **6 — UDM v1.1 deltas** | If §12 L9 items pan out, propose them as PRs against `specs/UDM-MagNET-v1.md`. | 1 day | none |
| **7 — Restful-vitals demo recording** | Capture a 30-second loop: subject sits/reclines in cone → presence appears → phase ribbons start streaming → second person walks in → second target glyph appears on the floor map → both leave → all marks fade. The phase-ribbon streamgraph + multi-target floor map are the hero shots. | 1 day | kit |

Total: roughly **3 weeks of focused work** plus shipping time on the kit.

Phases 0–4 are the minimum to call UC3 "shipped." Phase 5 is required before any sharing-of-the-dataspace work begins.

---

## 14. Decisions resolved (2026-05-03)

| # | Question | Decision |
|---|---|---|
| 1 | Project name | `MagNET_Vitals_E4TH` ✓ |
| 2 | UDM v1.1 candidates (`health-sensor`, `show-privacy`) | Approved ✓ |
| 3 | Hardware availability | Kit in hand — Phase 0 unblocked ✓ |
| 4 | Accuracy posture | Report-with-confidence (`signal_q` 0–100); renderer dims by confidence; demo framing is "restful vitals," not sleep ✓ |
| 5 | Multi-target exposure | Exposed as `targets` service + floor-plan scatter mark; privacy panel calls out the surface ✓ |

Phase 1 is unblocked. The next concrete milestone is the `craw_mr60bha2` UART driver against the kit's stock-firmware UART output (capture frames first; reconstruct the Tiny-Frame schema from the Seeed Arduino library; port to ESP-IDF).

---

*Cross-references: [`UDM-MagNET-v1.md`](./UDM-MagNET-v1.md), [`prototype/d3-spatial/examples/uc2-room.json`](../reference-designs/webxrofthings/prototype/d3-spatial/examples/uc2-room.json) (UC2 reference), [`MagNET_M5DialFiddlerCrab/M5Stamp3CU_Blinky_E4TH/`](../reference-designs/MagNET_M5DialFiddlerCrab/M5Stamp3CU_Blinky_E4TH/) (firmware family).*
