# ArtemisNano_Hive_Multisense — low-power multi-sensor hive member

**Status:** 🔧 **BRING-UP.** Platform proven 2026-10-18: Zephyr v4.4.2 boots from `0x10000` via the SVL
(phases P1–P2). Sensor and BLE work starts at M1 (§7).
**Build / flash:** `west build -b sparkfun_artemis_nano -d C:/zephyr-artemis/build/multisense <this dir>`
then `python scripts/flash.py --monitor 10` (Zephyr venv; workspace from `scripts/setup_zephyr_ws.sh`).
**Board:** [SparkFun RedBoard Artemis Nano](https://github.com/sparkfun/RedBoard_Artemis_Nano)
(DEV-15443): Artemis module = Ambiq **Apollo3 Blue** (Cortex-M4F @ 48/96 MHz, 1 MB flash,
384 KB SRAM, BLE 5), LiPo charger, Qwiic, PDM mic. USB-C via CH340E serial. Inventory: 3 working boards.
**Role:** **Role 15: Multisensor**, a node that carries more than one sensor (motion/orientation, light,
time, its own energy) and tells the hive which ones it has (§5).
**Platform:** upstream **Zephyr** + our out-of-tree board `sparkfun_artemis_nano` (§1–§3).
**Hive link:** BLE only (no WiFi/Thread on the Apollo3), §5.
**Forth:** yes, via a Zephyr platform layer for the ESPIDFORTH engine (§6).
**Previously:** this project was `ArtemisNano_Hive_Turret_Test`; the turret is parked (Appendix A).

### Bench log

| Date | Outcome |
|---|---|
| 2026-10-18 | Desk research: platform options, Zephyr port status, J-Link connector, servo selection. ULN2003/28BYJ-48 steppers parked in favor of the FS0307 servo. |
| 2026-10-18 | **P1 done.** Workspace `C:\zephyr-artemis` (Zephyr v4.4.2, SDK 1.0.1, Arm toolchain only) via `scripts/setup_zephyr_ws.sh`. `hello_world` (14,376 B) and `bluetooth/peripheral` (144 KB flash / 37 KB RAM) build green for `apollo3_evb`. Found the SoC default `FLASH_LOAD_OFFSET=0xC000` collides with the SVL (§1). |
| 2026-10-18 | P2 built: board `sparkfun_artemis_nano`, app linked at `0x10000` (vector table verified). `scripts/flash.py` speaks the SVL protocol natively: CRC matches SparkFun's on 2,000 random vectors, and it rejects the stock `apollo3_evb` image (reset vector `0xC951`). |
| 2026-10-18 | First Nano unreachable: no SVL, no ROM-bootloader (ASB) answer, only the red LED lit, RX/TX LEDs dark. **Root cause: its Artemis module had been physically torn off.** The CH340 and 3V3 rail still worked, so COM4 enumerated. Board retired. Lesson: if the SVL *and* the ROM bootloader are silent while COM4 enumerates, inspect the board. |
| 2026-10-18 | **P2 passed on hardware** (second Nano, no J-Link). SVL v5, 9 frames at 115200. Console: `Booting Zephyr OS build v4.4.2`, `VTOR=0x00010000`, tick 10 at 5021 ms (clocks correct). Blue LED blinking. One stray byte on TX at reset (harmless). |
| 2026-10-19 | Project repurposed: **Turret → Multisense** (low-power multi-sensor node). Design below. |
| 2026-10-19 | **M1 scan** (chain attached): `0x0B` LC709203F ✅, `0x44` OPT4048 ✅, `0x69` RV-1805 ✅, stable over 3 scans. AK9753 not seen at `0x64/65/67`; unexpected ACKs at `0x39 0x4F 0x62` (to recheck after the swap). **AK9753 dropped; replaced by BNO085 IMU** (`0x4A`). |
| 2026-10-19 | **M1 passed.** After the swap: `0x0B` LC709203F, `0x44` OPT4048, `0x4A` BNO085, `0x69` RV-1805, nothing else. The `0x39/0x4F/0x62` ACKs left with the presence board, which never answered as an AK9753 (wrong part or faulty board). |
| 2026-10-19 | **M2 on the bench.** RV-1805 reports `ID0=0x18 ID1=0x05` (AM1805 part number): the upstream `ambiq,am1805` driver works unmodified. Trickle charger enabled by `src/rv1805.c` (reads back `0xA5`); first boot found year 1900 (never set) and loaded the build time; **time kept across reset** ✅. LC709203F (APA 2000 mAh, 4.2 V profile): `vbat 4.067 V soc 87%` on USB charge. Requesting `FUEL_GAUGE_TEMPERATURE` without a thermistor fails the *whole* `get_props` call (`-134`), so it is not requested. Remaining checks: voltage against a meter; time across a full power-off on the supercap (allow ~10 min to charge first). Alarm waits for RV-1805 INT → D6. |
| 2026-10-19 | **M3 on the bench, first flash.** OPT4048 `ID=0x0821`; continuous auto-range at 100 ms, CRC checked on every channel: `15.2 lux, xy 0.398/0.379, CCT 3577 K` under room light. BNO085 via vendored CEVA SH-2 (`lib/sh2`, unmodified): `part 10004148 v3.2.13 build 6`; rotation vector at 10 Hz (~100 reports per 10 s) and stability classifier (`on-table`). **INT and RST not wired yet**: SH-2 runs polled every 10 ms with a soft reset at open. Heading accuracy reads ±180° until the magnetometer is calibrated (move the board in a figure-8). Battery temperature is a placeholder (`tbat n/a (no thermistor)`, sentinel `TBAT_NONE_DC`) until a 10k thermistor is fitted. Build: 52 KB flash, 26 KB RAM. |

---

## 1. Zephyr port status — what we learned

- **[zephyr#74116](https://github.com/zephyrproject-rtos/zephyr/issues/74116)** (AlienSarlak's
  Artemis Nano port) was an SVL upload failure, answered in
  [discussion #74478](https://github.com/zephyrproject-rtos/zephyr/discussions/74478): the **SVL
  rejects images whose length isn't a multiple of 4**. `flash.py` pads.
- **The load address.** The SVL lives at `0xC000` and runs applications from **`0x10000`**. The
  Apollo3 SoC default in v4.4.2 is `CONFIG_FLASH_LOAD_OFFSET=0xC000`
  (`soc/ambiq/apollo3x/Kconfig.defconfig.apollo3_blue`), **exactly where the SVL lives**: an
  unmodified Zephyr image won't boot from the SVL, and the same image flashed by J-Link overwrites
  it. Our board sets `0x10000` (`boards/sparkfun/artemis_nano/Kconfig.defconfig`).
- **Upstream supports the SoC.** `apollo3_evb` (same silicon) has the BLE HCI driver
  (`ambiq,bt-hci-spi`), CTIMER PWM, I2C (IOM), deep sleep (`suspend-to-ram`, ~125 µs exit). No fork needed.

## 2. Platform options

| Option | For | Against |
|---|---|---|
| **A. Upstream Zephyr + out-of-tree board** ✅ chosen | Maintained RTOS, BLE host, sensor/RTC/fuel-gauge driver classes, PM framework, `west` scripting. Same toolchain as `PetWear_Zephyros`. | We own a small board definition and two sensor drivers. |
| B. SparkFun Arduino_Apollo3 core v2 | SparkFun libraries exist for all four Qwiic parts | Built on Mbed OS (Arm EOL July 2026). Poor low-power control. Dead end for hive work. |
| C. AmbiqSuite SDK | Full control | Old Makefile tooling, nothing carries over to the fleet |

## 3. Debug and flash paths

### 3.1 USB (SVL): the normal path

`scripts/flash.py`: validates the image (initial SP in SRAM, reset vector inside `0x10000–0xFC000`),
pads to 4 bytes, uploads over the SVL protocol, exits non-zero on failure.
`--monitor N` prints the console afterwards. `--monitor-only` just resets and prints.

### 3.2 J-Link (SWD): optional, the footprint is not populated on our boards

Unpopulated **2×5, 1.27 mm through-hole** footprint J11 (verified from `RedBoard-Artemis-Nano.sch`):
1 = 3V3, 2 = SWDIO (pad 21), 3/5/9 = GND, 4 = SWDCK (pad 20), 6 = SWO (pad 33 = header A1), 10 = !RESET.
SWDIO/SWDCK also on test points TP2/TP1; neither is on the 0.1" headers.
Flying-lead wiring for a J-Link BASE: 1 VTref → 3V3 · 7 SWDIO → J11.2 · 9 SWCLK → J11.4 ·
15 RESET → RST · 4 GND → GND. Diagram: [`docs/wiring.html`](docs/wiring.html),
https://claude.ai/artifact/244kEtWoGoCCKFDKcuLoxq (its servo section is parked with Appendix A).
Worth soldering if BLE (M4) or deep sleep (M5) produce silent failures; not needed otherwise.

### 3.3 Flash map

| Range | Contents |
|---|---|
| `0x00000–0x0BFFF` | Ambiq secure bootloader (never write) |
| `0x0C000–0x0FFFF` | SparkFun SVL (never write; it is the USB recovery path) |
| `0x10000–0xFBFFF` | **Application** (Zephyr `code-partition`, 944 KB) |
| `0xFC000–0xFFFFF` | `storage` (16 KB): settings, installed role bundle, calibration |

## 4. Hardware

### 4.1 Sensors (one Qwiic / STEMMA QT daisy chain)

| Part | Board | Senses | I2C | Supply / current (vendor) | Zephyr driver |
|---|---|---|---|---|---|
| BNO085 | [Adafruit 4754](https://learn.adafruit.com/adafruit-9-dof-orientation-imu-fusion-breakout-bno085/arduino) | 9-DoF with on-chip fusion: rotation vector, accel, gyro, mag, step/tap/significant-motion, stability classifier | `0x4A` (DI high: `0x4B`) | VIN 3–5 V (on-board regulator, level shifters); **mA-range while fusing**, sleep modes available | ✅ CEVA SH-2 vendored in `lib/sh2` (Apache-2.0) + `src/bno085.c` HAL (I2C, polled; INT/RST next) |
| OPT4048 | [SparkFun SEN-24020](https://www.sparkfun.com/sparkfun-tristimulus-color-sensor-opt4048dtsr-qwiic.html) | CIE XYZ + clear channel → lux (2.15 mlux–144 klux), CCT, CIE xy | `0x44` (`0x45`/`0x46`) | 1.71–3.6 V, 24 µA active, 2 µA standby; 0.6–800 ms conversion | ✅ `src/opt4048.c` (app-level register driver; candidate for an upstream `ti,opt4048` sensor driver) |
| RV-1805 | [SparkFun BOB-14558](https://www.sparkfun.com/sparkfun-real-time-clock-module-rv-1805-qwiic.html) | Wall-clock time, alarms; supercap backup (~35 days) | `0x69` | ~22 nA (lowest-power mode) | ✅ `ambiq,am1805`: the RV-1805 is Micro Crystal's AM1805; confirmed on the bench (`ID0=0x18 ID1=0x05`) |
| LC709203F | [Adafruit 4712](https://learn.adafruit.com/adafruit-lc709203f-lipo-lipoly-battery-monitor/overview) | LiPo voltage, state of charge, optional 10k thermistor (not fitted: temperature reported as n/a) | `0x0B` | powered from the battery | ✅ `onnn,lc709203f` (fuel gauge API) |
| SPH0641 (on the Nano) | built in | Sound level (PDM mic, pads 36/37) | — | — | ✅ `dmic` (optional, power-hungry; off by default) |

- **No address conflicts.** Bus: IOM2 on the Qwiic connector (SCL pad 27, SDA pad 25), already in
  the board DTS as `qwiic_i2c`. Run it at **100 kHz** (conservative for the LC709203F; confirm its limit).
- **Pull-ups:** every breakout adds its own. Four sets in parallel load the bus and waste current
  whenever a line is low. Keep one set (on the Nano side) and cut the pull-up jumpers on the rest.
- **Interrupt lines** (all Apollo3 GPIOs can wake from deep sleep). Proposed, avoiding the
  BLEIF pads (31/32/33/35) and SWO (A1):

  | Signal | Nano pin | Pad |
  |---|---|---|
  | BNO085 INT (data ready, active low; required) | D8 | 38 |
  | BNO085 RST (active low) | D10 | 40 |
  | OPT4048 INT (lux threshold / conversion done) | D7 | 42 |
  | RV-1805 INT (alarm) | D6 | 43 |
  | LC709203F ALARM (low SOC/voltage) | D4 | 18 |

  Pin names on each breakout to confirm against its hookup guide.

### 4.2 Power

Battery: single-cell LiPo, **3.7 V nominal, 2000 mAh** (fuel-gauge pack size = 2000 mAh) → **LC709203F board (JST-PH pass-through)** → Nano JST battery
input (`V_BATT` → Schottky → `VIN` → AP2112 3.3 V). The Nano's MCP73831 charges from USB.

**Always-on loads on the Nano** (from the schematic). These set the sleep floor regardless of firmware:

| Load | Estimate | Fix |
|---|---|---|
| Red power LED D6 (3.3 V via 1 kΩ) | **~1.5 mA**, dominant | Remove D6 or R10 on field units |
| CH340E (powered from 3.3 V, not from USB) | unmeasured; suspect hundreds of µA to mA | Measure first; if large, field units need a cut trace or a different serial strategy |
| AP2112K-3.3 regulator | ~55 µA quiescent (datasheet typ.) | Accept, or switch power off with the RTC (below) |
| SPH0641 mic, idle without PDM clock | small (µA range) | Accept |
| Breakout power LEDs (if fitted) | ~1 mA each | Cut the LED jumpers |

The LED/jumper removals are deferred (2026-10-19): bench units keep them; they matter only for
battery life, and get revisited in M5 with measurements.

Two power architectures, chosen after measuring (M5):

- **A. Deep sleep (default).** Zephyr PM drops to `suspend-to-ram` between samples; sensors in
  standby / one-shot; wake on STIMER or the interrupt lines; BLE advertises at a slow interval.
  Keeps RAM, the Forth dictionary and the BLE identity. Floor = the table above.
- **B. RTC power switch (stretch).** The AM1805/RV-1805 power-switch output drives the Nano's
  `PSWC` pad (J4, the AP2112 `EN`, 10 kΩ pull-up to `VIN`). The RTC cuts the whole board's 3.3 V
  between samples (tens of nA) and an alarm powers it back on. Cold boot every cycle (~100s of ms),
  so the hive sees a periodic visitor, not a resident. **Open question:** the PSW pin sees `VIN`
  (up to 4.2 V) through R1 while the RTC runs from its supercap; check the RV-1805 pin limits first.

## 5. Hive integration — Role 15: Multisensor

Named 2026-10-19. Not yet in the top-level role list (README Roles 1–14); proposed wording:
*"Multisensor: any device that carries more than one sensor, advertises which sensors it has, and
shares their readings with the hive."* Open design item: **how it advertises what it has** (§5.1).

Transport is BLE only:

1. **Broadcast:** sensor readings in BLE advertisements (connectionless, cheapest radio mode).
   Any listener (ruler, `magnet_app`, a Hanasu C6 node acting as a gateway) picks them up.
2. **Connect on demand:** a GATT service speaking the **HCP subset** Hanasu already uses over BLE:
   `WHOAMI`, `SYSINFO`, KV, and the privileged `BUNDLE BEGIN/ADD/COMMIT` for signed role bundles.
3. **Time:** the RV-1805 keeps wall-clock through sleep and power-off. The node can accept the
   hive's mesh time and could later serve as a time anchor.

### 5.1 Advertising what it has (proposal, 2026-10-19)

**Fleet today** (repo survey): no MagNET firmware sets manufacturer or service data. Hanasu adverts
are flags + the 128-bit HCP service UUID `6d61676e-2d68-6370-0001-000000000000` (21 of 31 bytes),
with the name `MagNET-%02x%02x` in the scan response (`MagNET_Thread_COaP_hanasu_esp32c6/firmware-idf/components/magnet/magnet_ble.c:340-389`).
`magnet_app` matches the name prefix `MagNET-` or that UUID (`magnet_app/lib/magnet/magnet_ble.dart:40-58`).
Capabilities exist only after connecting (`CAPS`, `WHOAMI`, `STATUS`), and they list protocol
features, not hardware. Hanasu already has a **Zephyr port** (`firmware-zephyr/src/magnet_ble_zephyr.c`)
with the same GATT service: the starting point for M4/M7.

**Layer 1 — advert (any scanner, no connection).** Keep the Hanasu layout so existing tools find
the node; spend the 10 spare bytes on manufacturer data (company ID `0xFFFF` = testing, for the
prototype):

| Byte | Field |
|---|---|
| 0 | format version (hi nibble) · role number (lo nibble; Multisensor = 15) |
| 1–2 | sensor bitmap: b0 presence (none fitted) · b1 lux · b2 color (XYZ/CCT) · b3 time (RTC) · b4 battery gauge · b5 sound · b6 orientation/motion (IMU) · b7–15 reserved |
| 3 | battery % (0xFF = no gauge) |
| 4 | reading sequence number (new data vs. stale) |
| 5 | installed role-bundle tag (low byte of the bundle hash; 0 = none) |

Latest readings (presence state/direction, lux, CCT) go in the scan response next to the name.
**The bitmap is detected, not hard-coded:** set from the boot-time bus probe, so a missing or
loose breakout clears its bit.

**Layer 2 — connected (HCP).** `CAPS` gains `role=multisensor sensors=orientation:bno085,motion:bno085,lux:opt4048,cct:opt4048,time:rv1805,battery:lc709203f`
(names shared with the WiFi hive's `caps[]` vocabulary, so a RoleBundle's `caps_req` can be checked
against them), and a `SENSE` verb returns current readings as key=value.

Open: register a real Bluetooth SIG company ID before anything ships; optionally also emit
**BTHome v2** service data so Home Assistant can read the sensors without MagNET software.

Example hive behaviors, as bundles rather than firmware: significant motion + darkness → wake the Beeper;
low SOC → lengthen the sample interval and tell the ruler; CCT/lux logging for a Scribe.

## 6. Forth on the Artemis

**Yes.** The ESPIDFORTH engine (`components/forth/forth_core.cpp`, 1,456 lines, C API in
`forth_core.h`: `forth_init`, `forth_eval`, `forth_eval_rollback`, `forth_register_word`, …) is
portable C++. Its ESP-IDF dependencies are confined to:

| Used for | ESP-IDF | Zephyr replacement |
|---|---|---|
| Time words | `esp_timer` | `k_uptime_get` / `k_cyc` |
| Memory/diagnostic words | `heap_caps_*`, `esp_get_free_heap_size` | `sys_heap` runtime stats |
| Chip-info / MAC words | `esp_chip_info`, `esp_efuse_mac_get_default` | `hwinfo_get_device_id`, BT address |
| Re-entrancy lock (role-tick vs. bundle install) | FreeRTOS mutex | `k_mutex` |

Plan: add a small platform layer (`forth_platform.h` with `esp-idf` and `zephyr` backends) **upstream
in IoTone/ESPIDFORTH**, consume it as a Zephyr module, and register Multisense words:
`quat@ ( -- w x y z )`, `motion?`, `lux@`, `cct@`, `xyz@`, `soc@`, `vbat@`, `now@`, `alarm!`,
`sample-ms!`. Role bundles define `role-init` / `role-tick` exactly as on the ESP32 fleet.
Bundle verification reuses `magnet_crypto` (tweetnacl Ed25519, portable) and SHA/HMAC from
Zephyr's mbedtls/PSA. Resources are not a concern: 384 KB SRAM and 944 KB app flash.

## 7. Phases

Platform phases P1–P2 are done (bench log). Multisense phases:

| # | Goal | Gate |
|---|---|---|
| M1 | Qwiic bus up (`qwiic_i2c` enabled, 100 kHz); bus scan | All four addresses ACK: `0x0B 0x44 0x4A 0x69` |
| M2 | In-tree drivers: RV-1805 via `ambiq,am1805` (set/get time, alarm interrupt), LC709203F (voltage, SOC) | Time survives a reset; SOC and voltage plausible against a meter |
| M3 | OPT4048 driver (XYZ → lux, CIE xy, CCT) in `src/opt4048.c`; BNO085 via CEVA SH-2 + Zephyr HAL. **Polled version working**; INT-driven version once D8/D10 are wired | Lux tracks a phone light meter; rotation vector follows the board by hand; significant-motion event fires |
| M4 | **BLE** (highest risk): Ambiq HCI up; resolve the EVB's BLEIF pinmux vs. header pins A1/A5/A14/A15; advertise a sensor payload | Phone sees adverts with live readings |
| M5 | **Power:** PM `suspend-to-ram`, sensors in one-shot/standby, interrupt wake; measure; LED/jumper mods; decide architecture A vs. B | Measured average current against a target set after the first measurement |
| M6 | Forth: ESPIDFORTH platform layer + Multisense words; REPL on the console | `presence@ . lux@ .` prints live values |
| M7 | Hive: HCP over GATT, signed `BUNDLE` install, role lifecycle | A signed Multisensor bundle installs over BLE and its `role-tick` reports readings |

## Appendix A — parked: turret (FS0307 servo)

Set aside 2026-10-19 because the platform is better suited to low power. Kept for a future board
from the inventory.

- Servo: [FEETECH FS0307](https://www.pololu.com/product/3420), positional, ~120° on 900–2100 µs,
  50 Hz, 3.3 V signal OK, 4.8–6 V supply (separate from the Nano's 3V3; 470 µF across it; common ground).
- Signal on **D9 = pad 39 = CTX25**, driven by **timer 2 segment B** (B2OUT; see `outcfg_tbl` in
  hal_ambiq `am_hal_ctimer.c`). Already in the board DTS as `pwm2` (disabled). Timer 2 must move to
  `CLK_SELECT_HFRC_3MHZ` so a 20 ms frame (60,000 counts) fits the 16-bit segment.
- Verbs planned: `AIM <deg>`, `CENTER`, `SWEEP <lo> <hi> <ms>`, `POS`, `RELAX`.
- Also parked: ULN2003 + 28BYJ-48 stepper (3.3 V logic OK, own 5 V supply, 4 GPIOs per motor).
