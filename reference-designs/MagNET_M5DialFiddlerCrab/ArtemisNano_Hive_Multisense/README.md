# ArtemisNano_Hive_Multisense — low-power multi-sensor hive member

**Status:** 🔧 **BRING-UP.** Platform proven 2026-10-18 (P1–P2). M1–M2 passed; M3 sensors reporting (IMU
INT/RST wiring and hand checks pending); M4 BLE advert + HCP GATT working from the PC and **readable in
`magnet_app`** (§7). Upstreaming the board to Zephyr: §8.
**Build / flash:** `west build -b sparkfun_artemis_nano -d C:/zephyr-artemis/build/multisense <this dir>`
then `python scripts/flash.py --monitor 10` (Zephyr venv; workspace from `scripts/setup_zephyr_ws.sh`).
**BLE check from the PC:** `python scripts/ble_probe.py` (scan + decode) or `--hcp WHOAMI CAPS SENSE` (needs
`pip install bleak`; on Windows keep the venv on a short path such as `C:/zephyr-artemis/blevenv`; the WinRT DLLs
fail to load from long paths).
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
| 2026-10-19 | **M1 scan** (chain attached): `0x0B` LC709203F ✅, `0x44` OPT4048 ✅, `0x69` RV-1805 ✅, stable over 3 scans. AK9753 not seen at `0x64/65/67`; unexpected ACKs at `0x39 0x4F 0x62` (to recheck after the swap). **AK9753 dropped; replaced by BNO085 IMU** (`0x4A`). **(`0x62` was misread — see 2026-10-29.)** |
| 2026-10-19 | **M1 passed.** After the swap: `0x0B` LC709203F, `0x44` OPT4048, `0x4A` BNO085, `0x69` RV-1805, nothing else. The `0x39/0x4F/0x62` ACKs left with the presence board, which never answered as an AK9753 (wrong part or faulty board). **This conclusion was wrong for `0x62` — see 2026-10-29.** |
| 2026-10-19 | **M2 on the bench.** RV-1805 reports `ID0=0x18 ID1=0x05` (AM1805 part number): the upstream `ambiq,am1805` driver works unmodified. Trickle charger enabled by `src/rv1805.c` (reads back `0xA5`); first boot found year 1900 (never set) and loaded the build time; **time kept across reset** ✅. LC709203F (APA 2000 mAh, 4.2 V profile): `vbat 4.067 V soc 87%` on USB charge. Requesting `FUEL_GAUGE_TEMPERATURE` without a thermistor fails the *whole* `get_props` call (`-134`), so it is not requested. Remaining checks: voltage against a meter; time across a full power-off on the supercap (allow ~10 min to charge first). Alarm waits for RV-1805 INT → D6. |
| 2026-10-19 | **M3 on the bench, first flash.** OPT4048 `ID=0x0821`; continuous auto-range at 100 ms, CRC checked on every channel: `15.2 lux, xy 0.398/0.379, CCT 3577 K` under room light. BNO085 via vendored CEVA SH-2 (`lib/sh2`, unmodified): `part 10004148 v3.2.13 build 6`; rotation vector at 10 Hz (~100 reports per 10 s) and stability classifier (`on-table`). **INT and RST not wired yet**: SH-2 runs polled every 10 ms with a soft reset at open. Heading accuracy reads ±180° until the magnetometer is calibrated (move the board in a figure-8). Battery temperature is a placeholder (`tbat n/a (no thermistor)`, sentinel `TBAT_NONE_DC`) until a 10k thermistor is fitted. Build: 52 KB flash, 26 KB RAM. |
| 2026-10-19 | M3: significant-motion (one-shot) enabled and re-armed after each event; reads `sigmo=0` at rest, firing still to be checked by moving the board. |
| 2026-10-19 | **M4 BLE up on the first flash.** The radio is inside the Artemis module (internal BLEIF); Zephyr only drives the EVB's BLEIF pads with `CONFIG_SPI_AMBIQ_BLEIF_TIMING_TRACE`, so **no header pins are used** and A1/A5/A14/A15 stay free. Controller: HCI 5.0, manufacturer `0x09ac`, identity `66:77:88:23:BB:EF` (public, from the controller). Node ID `029d7d8f` (FNV-1a of the Apollo3 chip ID); name `MagNET-7d8f`. Bonds in NVS on the `storage` partition (2 x 8 KB). Build: 170 KB flash, 58 KB RAM. |
| 2026-10-19 | **M4 checked from the PC** (`scripts/ble_probe.py`, bleak over the PC's adapter): advert decodes as role 15, sensors lux/color/time/battery/orientation (bitmap `0x5e`), battery 96%, live lux/CCT/yaw/stability from the scan response. GATT HCP: `PING WHOAMI STATUS CAPS SENSE` all answer; an unknown verb returns `-ERR E_UNKNOWN`; the node logged ATT MTU tx 527 / rx 247 and the console mirrors every `hcp<`/`hcp>` line. Removed Hanasu's on-connect Security Request: Windows turned it into a pairing prompt that cancelled the first write (`WinError -2147023673`). One SVL upload failed (`frame 7/84 rejected 4 times`, cause unknown); the immediate retry was clean. Phone/`magnet_app` check pending (the Galaxy did not show in `adb devices`). |
| 2026-10-19 | **M4 checked on the phone** (Galaxy S20 FE, SM-G780G, `magnet_app` debug build from `dev`): the Devices list shows `MagNET-7d8f` with the MagNET node badge; the node screen reads `READY`, role `multisensor`, id `029d7d8f`, fw `0.4.0-m4`, channel `none`. Two firmware fixes were needed: (1) the app's refresh also sends `CHANNEL SHOW` and `ADMIN LIST`, and an `-ERR` there left every field at `?`, so both now answer (no channel, empty allow-list); (2) **notification chunking bug**: a line exactly `MTU-3` long had its `
` appended in the same notification, `bt_gatt_notify` rejected it and the reply vanished (CAPS with the app's `@a0005` tag hit this). Fixed with a strict `<`. Hanasu's `magnet_ble_zephyr.c` has the same `n <= chunk` line. Note: opening the serial monitor resets the board (RTS is wired to RESET), which drops any BLE link. The app does not display the advert readings yet; it would need a decoder for the §5.1 bytes or a `SENSE` panel. |
| 2026-10-29 | **`magnet_app` shows the readings.** New `lib/magnet/multisense.dart` decodes the §5.1 manufacturer block, so the Devices list prints live `lux · CCT · yaw · SOC` under any multisensor **without connecting**; the node screen gained a Sensors card fed by a 5 s `SENSE` poll (`HcpClient.sense()`), with the heading-accuracy hint when the magnetometer is uncalibrated. Unit tests cover both decoders including the `0xFFFF`/`0x8000` sentinels and the 6-byte header-only advert (`test/magnet/multisense_test.dart`). Also fixed: **the node screen never watched the BLE link**, so a reboot — a reflash, or the battery taking over from USB — left it showing stale values while every command failed into a snackbar. It now watches `connectionState`, shows a banner, and retries the same address for 3 minutes (1/2/3/5 s backoff), the same policy `MeshSession` already used. |
| 2026-10-29 | **Blue LED now says whether anyone is connected** (fw `0.4.1-m4`, **not yet built or flashed** — the Zephyr workspace is on the Windows bench): 1 Hz even blink while advertising, an 80 ms pulse every 2.5 s once a central connects (`hive_ble_connected()` is new). The main loop was `toggle + k_msleep(500)` with the 10 s report counted in LED ticks; it now runs the LED and the report off two independent wall-clock deadlines, so a variable LED period neither drifts the report nor gets its phase stretched by a sample. Connected is also the cheaper pattern (~3% duty vs 50%), which helps M5. |
| 2026-10-29 | `magnet_app` **release** build installed on the Galaxy S20 FE (`RF8R71GWDYL`). `flutter install` uninstalled the old debug build first, so app storage was wiped: **this phone has a new operator key**, and any node whose allow-list holds the old one must be re-enrolled. The Artemis is unaffected — its allow-list is still empty. |
| 2026-10-29 | **Bench test from the Mac** (`scripts/ble_probe.py` + bleak, node at -44 dBm). Two findings. (1) **The OPT4048 is detected but not reading.** `CAPS` lists `lux:opt4048,cct:opt4048` — so the boot probe ACKed `0x44` *and* `opt4048_init()` returned 0 — while `SENSE` omits `lux=`/`cct=` entirely and the advert carries the `0xFFFF` sentinel in both fields. So `opt4048_read()` is failing on every sample; this is a regression since M3 (2026-10-19, `ID=0x0821`, 15.2 lux) and is **not** a loose breakout: battery, IMU and RTC all answer on the same bus. Next: serial monitor for the per-sample path, and whether M4's BLE changed bus timing. (2) **The §5.1 readings offset was wrong in both decoders** — see the note in §5.1. Everything else checks out: `WHOAMI` id `029d7d8f`, fw `0.4.0-m4`, MTU 247, `SENSE` `yaw=172.0 pitch=-4.3 roll=-6.1 motion=stable sigmo=5 vbat=4.184 soc=98`, RTC `2026-10-29T13:46:24`. |
| 2026-10-29 | **The "faulty" presence board was probably answering correctly.** `0x62` is the documented I2C address of the **Useful Sensors Person Sensor** (confirmed in the vendor developer guide and in ESPHome's `sen21231` driver: `PERSON_SENSOR_I2C_ADDRESS = 0x62`). M1 probed `0x64/0x65/0x67` for an AK9753, got nothing, saw an ACK at `0x62`, and recorded the board as the wrong part or faulty — but `0x62` is exactly where a Person Sensor answers. It was then pulled from the chain. `0x39` and `0x4F` are still unexplained: a single device should not ACK three addresses, so either something else was on the chain or those were phantom ACKs from the four sets of parallel Qwiic pull-ups (§4.1). Added `0x62` to `known[]` (caps 0 — no driver yet) and an opt-in full-bus sweep (`MS_BUS_SWEEP` in `src/main.c`, default 0) so an address nobody expected is named rather than guessed at. **Note `MS_CAP_PRESENCE` is defined in `multisense.h` and assigned nowhere**, so presence cannot reach `CAPS`, `SENSE` or the advert even with the board fitted. Hardware check before any driver work: the sensor ships in continuous mode with its debug LED on, so powering it at 3.3 V and showing it a face should light the LED green — no firmware needed. |

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

**Person Sensor (Useful Sensors, v1.1) — candidate, not fitted.** `0x62`, 3.3 V only. Face
detection and optional recognition (up to 4 faces, 8 stored IDs); results are a **39-byte plain I2C
read with no register pointer** (a register-style read fails — the likeliest way a new driver looks
dead against a healthy board), while writes are register-style `[reg, value]`. Defaults: continuous
mode (`0x01`=1, ~7 Hz, ~200 ms latency) and debug LED on (`0x07`=1). v1.0/v1.1/v2.0 are
software-compatible; v1.1's changes are mechanical (snap-off panel, individual signal pads, camera
module glued rather than press-fit). **The catch is power: ~150 mW, about 45 mA at 3.3 V** — roughly
1800x the OPT4048's 24 µA and far beyond the red power LED (1.5 mA) that §4.2 calls the dominant
sleep load. Running it continuously sets the floor for the whole node and makes both M5 power
architectures moot while it is awake, so it has to be duty-cycled from standby (`0x01`=0) via
single-shot (`0x03`), most naturally on the BNO085's significant-motion event. Undocumented in the
sources checked: the CRC16 parameters (the vendor's `person_sensor_pico_c` driver is authoritative),
the endianness of `data_size`/checksum, and whether bytes 8/9 are right/bottom or width/height.

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

**Unplugging USB resets the board** (suspected, 2026-10-29). `V_BATT` reaches `VIN` through a
Schottky, so the changeover dips the rail below the AP2112's headroom and the Apollo3 brown-outs —
which drops BLE, drops the hive membership, and restarts the sensors. `magnet_app` now survives this
(it reconnects, §5), but the node side is unconfirmed. **To confirm rather than assume:** report
`hwinfo_get_reset_cause()` in `STATUS` (and clear it at boot) — `hwinfo` is already linked for the
node id — then watch whether a changeover shows `BROWNOUT`/`POR` and how long the gap is. If it does,
the fixes are ordinary: more bulk capacitance on `VIN`, or accept the reboot and make it cheap (the
RTC already keeps wall-clock time across it, §4.1, and the BLE identity and bonds survive in NVS).

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

Latest readings go in the scan response next to the name. **As built (M4):** the scan response carries
manufacturer data under the same `0xFFFF`, holding the 6-byte header again plus 8 bytes of readings,
because hosts merge advert and scan response per company ID (a readings-only block would hide the header):

| Byte | Field |
|---|---|
| 0–5 | header, as above |
| 6–7 | lux, u16 LE, saturating (`0xFFFF` = no reading) |
| 8–9 | CCT in K, u16 LE (`0xFFFF` = no reading) |
| 10–11 | yaw in degrees, s16 LE (`0x8000` = no reading) |
| 12 | stability: 0 unknown, 1 on-table, 2 stationary, 3 stable, 4 motion (`0xFF` = none) |
| 13 | significant-motion count, low byte |

Name (13 bytes) + this block (18 bytes) fills the 31-byte scan response exactly.

> **Decoders must take the readings from the last 8 bytes, not offset 6.** Measured off the air
> 2026-10-29: the host **concatenates** the two manufacturer blocks rather than replacing one with
> the other, so a scanner sees **20 bytes** — the 6-byte header *twice*, then the readings:
>
> ```
> 1f 5e 00 62 84 00 | 1f 5e 00 62 84 00 | ff ff ff ff ab 00 03 05
>    header               header again       lux   cct  yaw  st sig
> ```
>
> `length - 8` is correct for both shapes (offset 6 for a lone 14-byte scan response, 12 for the
> concatenated 20). A fixed offset 6 decodes the repeated header as data and reports `lux` 24095,
> `cct` 25088 — plausible-looking numbers that are really the header's own bytes. Both
> `scripts/ble_probe.py` and `magnet_app`'s decoder had this bug; fixed 2026-10-29.
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
| M3 | *IMU test rig idea:* once the Turret exists, it can turn the Multisensor through known angles so the hive checks yaw against commanded position (two members collaborating). OPT4048 driver (XYZ → lux, CIE xy, CCT) in `src/opt4048.c`; BNO085 via CEVA SH-2 + Zephyr HAL. **Polled version working**; INT-driven version once D8/D10 are wired | Lux tracks a phone light meter; rotation vector follows the board by hand; significant-motion event fires |
| M4 | **BLE**: Ambiq HCI up; BLEIF pins (resolved: none used); §5.1 advert + readings; Hanasu HCP GATT service with a built-in `PING/HELP/WHOAMI/STATUS/CAPS/SENSE` dispatcher. **Working from the PC and in `magnet_app` on the phone**, readings shown in the list and on the node screen (2026-10-29) | Phone sees adverts with live readings ✅ |
| M5 | **Power:** PM `suspend-to-ram`, sensors in one-shot/standby, interrupt wake; measure; LED/jumper mods; decide architecture A vs. B | Measured average current against a target set after the first measurement |
| M6 | Forth: ESPIDFORTH platform layer + Multisense words; REPL on the console | `presence@ . lux@ .` prints live values |
| M7 | Hive: HCP over GATT, signed `BUNDLE` install, role lifecycle | A signed Multisensor bundle installs over BLE and its `role-tick` reports readings |

## 8. Upstreaming the Artemis Nano support to Zephyr

**What we actually changed:** nothing inside Zephyr. The whole port is the out-of-tree board
`boards/sparkfun/artemis_nano/` (8 files, §1) plus two app-level sensor drivers and
`scripts/flash.py`. `hal_ambiq`, `soc/ambiq/apollo3x` and the Apollo3 BLE HCI driver are used
unmodified — that is the single most important fact for upstreaming, because it means there is no
fork to reconcile and the submission is purely additive.

Upstream has no Artemis board today. [zephyr#74116](https://github.com/zephyrproject-rtos/zephyr/issues/74116)
and [discussion #74478](https://github.com/zephyrproject-rtos/zephyr/discussions/74478) are one
earlier attempt that stopped at the SVL upload failure (a personal `new_board/artemis_nano` fork,
never a PR). So our port would be the first, and we would be its maintainer — Zephyr requires a
named maintainer with the hardware in hand for every board. We have three boards (one retired).

### 8.1 Order of work

| Stage | Deliverable | Risk | Why in this order |
|---|---|---|---|
| **0** | Rebase the board onto Zephyr `main` and keep it building there | low | Upstream only takes PRs against `main`; we develop on the `v4.4.2` tag. Do this first or every later stage is rework. |
| **1** | **PR: `boards: sparkfun: add RedBoard Artemis Nano`** | low | The one piece with real value to anyone else, and purely additive. |
| **2** | **PR: SVL west runner** (`west flash -r svl`) | medium | Makes the board flashable out of the box with no J-Link. Reviewers *will* ask; see §8.3. |
| **3** | **PR: `ti,opt4048` sensor driver** | medium-high | Useful to the whole ecosystem, but needs an answer on channels (§8.4). |
| **4** | BNO085 / CEVA SH-2 | high | Needs a new west module, not a driver PR. Stays out-of-tree; see §8.5. |

### 8.2 Stage 1 — the board PR

What we have already matches the upstream layout (`board.yml`, `Kconfig.sparkfun_artemis_nano`,
`Kconfig.defconfig`, `sparkfun_artemis_nano_defconfig`, the DTS + pinctrl dtsi, `board.cmake`,
`sparkfun_artemis_nano.yaml`). What has to be added or cleaned up:

- **Docs.** `boards/sparkfun/artemis_nano/doc/index.rst`: supported-features table, the flash map
  (§3.3), the `0xC000` vs `0x10000` trap written up properly (it is the thing that wastes everyone
  else's week), programming via SVL *and* via J-Link on J11, plus a board photo under `doc/img/`.
- **Drop the project-specific bits.** The parked servo `pwm2` node and `pwm2_default` pinctrl group
  carry turret comments (Appendix A). Keep the PWM node — exposing a header pin's timer is normal —
  but rewrite the comments to describe pad 39 / timer 2 segment B, not our servo.
- **`vendor: sparkfun`** already exists upstream (`sparkfun_pro_micro_rp2040` and friends), so no
  `vendor-prefixes.txt` change is needed.
- **Twister.** `sparkfun_artemis_nano.yaml` must be honest about `supported:`; `bluetooth` belongs
  in the list now that M4 works. Confirm `samples/hello_world`, `samples/basic/blinky` and
  `tests/kernel/common` build, and that `west build -b sparkfun_artemis_nano` is clean on `main`.
- **Process.** DCO `Signed-off-by`, one logical commit, and a compliance-clean PR (checkpatch, DTS
  lint, `scripts/ci/check_compliance.py`).

### 8.3 Stage 2 — the SVL runner

Upstream's instinct for a board whose debug footprint is unpopulated is "document the bootloader",
but `west flash` working with nothing but the USB-C cable is what makes the board pleasant, and we
already own a correct implementation: `scripts/flash.py` speaks the SVL protocol natively and its
CRC was checked against SparkFun's on 2,000 random vectors (bench log, 2026-10-18). Port it to
`scripts/west_commands/runners/svl.py` (a `ZephyrBinaryRunner` subclass), add the usual
`scripts/tests/` unit test, then make `svl` the board's default runner in `board.cmake` with
`jlink` as the alternative. Carry over the two hard-won behaviours: **pad the image to a multiple
of 4** (the SVL rejects anything else — the cause of #74116) and **validate before upload**
(initial SP in SRAM, reset vector inside `0x10000–0xFC000`), which is what stops a stock
`apollo3_evb` image from bricking the recovery path.

### 8.4 Stage 3 — the OPT4048 driver

`src/opt4048.c` is an app-level register driver; upstream wants `drivers/sensor/ti/opt4048/` with a
`ti,opt4048.yaml` binding, Kconfig, and `fetch`/`channel_get` over the sensor API. The design
question to settle *before* writing the PR: Zephyr has `SENSOR_CHAN_LIGHT` for lux but nothing for
CIE XYZ, CIE xy or CCT. Proposed answer — lux on `SENSOR_CHAN_LIGHT`, everything else as
driver-specific channels above `SENSOR_CHAN_PRIV_START`, documented in the binding; a new *generic*
colour channel is an API change and a long bikeshed we should not make a prerequisite. Keep the
per-channel CRC check (counter + CRC on every register read) — it is the part of this part worth
having and no other Zephyr light driver does it. Note the Linux IIO driver by Paul Kocialkowski as
prior art for the register map and auto-range handling.

### 8.5 Stage 4 — BNO085, and what stays ours

The BNO085 is driven by CEVA's SH-2 library, vendored unmodified in `lib/sh2` (Apache-2.0). Zephyr
does not take third-party trees into `drivers/`; this would have to become a west module
(`modules/hal/ceva` shape) with a thin sensor driver on top, which needs upstream buy-in before any
code is written. **Not worth starting now.** It costs us nothing to keep out-of-tree, and the
vendored copy is already pinned (`lib/sh2/VENDORED.txt`).

Also staying ours on purpose: the ESPIDFORTH Zephyr platform layer goes upstream in
**IoTone/ESPIDFORTH**, not Zephyr (§6), and `magnet_crypto`/HCP are MagNET, not of general interest.

### 8.6 Until it is merged

Zephyr board PRs take weeks to months. In the meantime:

1. **Keep this directory the source of truth** and consume it via `BOARD_ROOT`, as today.
2. **Publish it as a Zephyr board module** (`zephyr/module.yml` with a `board_root` setting) in its
   own small repo, so `PetWear_r1_zephos` and anyone else can `west` it in without copying files.
   Cheap, and it is also the thing to point a reviewer at.
3. **Comment on [#74116](https://github.com/zephyrproject-rtos/zephyr/issues/74116)** with the two
   findings that unblock it (the `FLASH_LOAD_OFFSET` collision and the 4-byte padding) and state the
   intent to upstream. That is how a reviewer gets interested early and how we avoid colliding with
   the existing fork.
4. **Do not let the two copies drift.** Once the board is merged, delete the local copy, pin a
   Zephyr revision at or after the merge in `scripts/setup_zephyr_ws.sh`, and keep only
   `app.overlay` here.

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
