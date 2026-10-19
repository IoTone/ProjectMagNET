# ArtemisNano_Hive_Turret_Test — bring-up plan

**Status:** 📋 **PLANNED.** Desk research 2026-10-18. No code written yet.
**Board:** [SparkFun RedBoard Artemis Nano](https://github.com/sparkfun/RedBoard_Artemis_Nano)
(DEV-15443) — Artemis module = Ambiq **Apollo3 Blue** (Cortex-M4F @ 48/96 MHz, 1 MB flash,
384 KB SRAM, BLE 5). USB-C via CH340E serial; no native USB.
**Candidate role:** turret / actuator node — aims a hobby servo on command from the hive.
**Actuator:** [FEETECH FS0307 sub-micro servo](https://www.pololu.com/product/3420) (Pololu #3420).
**Platform choice:** upstream **Zephyr** with an out-of-tree board definition (§2).
**Blocker:** none known. No WiFi/Thread, so hive membership is over **BLE** (§4).

### Bench log

| Date | Outcome |
|---|---|
| 2026-10-18 | Desk research: platform options, Zephyr port status, J-Link connector, servo selection. ULN2003/28BYJ-48 steppers parked in favor of the FS0307 servo. |
| 2026-10-18 | **Phase 1 done.** Workspace `C:\zephyr-artemis` (Zephyr v4.4.2, SDK 1.0.1, Arm toolchain only) via `scripts/setup_zephyr_ws.sh`. `hello_world` (14,376 B) and `bluetooth/peripheral` (144 KB flash / 37 KB RAM) build green for `apollo3_evb`. Found the SoC default `FLASH_LOAD_OFFSET=0xC000` collides with the SVL (§1). |

---

## 1. Zephyr port status — what we learned

- **[zephyr#74116](https://github.com/zephyrproject-rtos/zephyr/issues/74116)** (AlienSarlak's
  Artemis Nano port, fork branch `new_board/artemis_nano`) was an SVL upload failure. It was
  answered in [discussion #74478](https://github.com/zephyrproject-rtos/zephyr/discussions/74478):
  the **SparkFun Variable Loader (SVL) rejects images whose length isn't a multiple of 4**.
  His `zephyr.bin` was 14,603 bytes, so the last frame failed. Fix: pad the image.
- **Problem #2 the thread never reached: the load address.** The SVL lives at the bottom of
  flash and runs applications from **`0x10000`**. Confirmed in v4.4.2: the Apollo3 SoC
  default is `CONFIG_FLASH_LOAD_OFFSET=0xC000` (`soc/ambiq/apollo3x/Kconfig.defconfig.apollo3_blue`),
  directly after the Ambiq secure bootloader, **which is exactly where the SVL lives**. So an
  unmodified Zephyr image uploaded over the SVL runs from the wrong address and won't boot, and the
  same image flashed by J-Link overwrites the SVL. Our board sets `CONFIG_FLASH_LOAD_OFFSET=0x10000`.
- **Upstream already supports the SoC.** Zephyr's
  [`apollo3_evb`](https://docs.zephyrproject.org/latest/boards/ambiq/apollo3_evb/doc/index.html)
  (Apollo3 Blue, same silicon as Artemis) has a BLE HCI driver (`ambiq,bt-hci-spi`) and
  CTIMER PWM ([`ambiq,ctimer-pwm`](https://docs.zephyrproject.org/latest/build/dts/api/bindings/pwm/ambiq%2Cctimer-pwm.html)).
  The fork isn't needed. An Artemis Nano board is the EVB definition plus our
  pinmux, the `0x10000` code partition, and an SVL flash runner.

## 2. Platform options

| Option | For | Against |
|---|---|---|
| **A. Upstream Zephyr + out-of-tree board** ✅ chosen | Maintained RTOS with a BLE host stack, PWM servo support, and `west` scripting. Same toolchain as `PetWear_Zephyros`. | We own a small board definition. |
| B. SparkFun Arduino_Apollo3 core v2 | Fastest path to blinking an LED and moving a servo | Built on Mbed OS, which Arm was ending in July 2026. Dead end for hive work. |
| C. AmbiqSuite SDK (SparkFun fork) | Full control. Includes BLE examples and SparkFun board files. | Old Makefile tooling, and none of it carries over to the rest of the fleet. |

B is acceptable only as a 15-minute "does the servo move" smoke test.

## 3. Debug and flash paths

### 3.1 J-Link (SWD)

The Nano has an **unpopulated 2×5, 1.27 mm (0.05") Cortex-debug footprint**. Pin 1 is marked
by a small white line. Users report it working with a J-Link through a 20→10 adapter
([SparkFun forum](https://community.sparkfun.com/t/how-to-download-an-debug-redboard-nano-with-j-link/43518)).

Parts:
- **Header:** 2×5 1.27 mm SMD shrouded box header (e.g. Samtec FTSH-105-01-L-DV-K, or
  Adafruit #752). Shrouded, so the cable can only go in one way.
- **Adapter:** depends on your J-Link:
  - J-Link BASE/PLUS/EDU (20-pin 2.54 mm): **SEGGER 9-pin Cortex-M adapter** (8.06.02), or
    **Adafruit #2094** (20-pin 2.54 mm → 10-pin 1.27 mm) plus a 10-pin 1.27 mm ribbon.
  - J-Link EDU Mini / BASE Compact: already 10-pin 1.27 mm. Use only the ribbon.
- **Footprint verified** from `Hardware/RedBoard-Artemis-Nano.sch` (part J11,
  `2X5-PTH-1.27MM`, through-hole): 1 = 3V3, 2 = SWDIO (pad 21), 3/5/9 = GND,
  4 = SWDCK (pad 20), 6 = SWO (pad 33, shared with header A1), 10 = !RESET. SWDIO/SWDCK are
  also on test points TP2/TP1. Neither is on the 0.1" headers.
- **Flying-lead wiring, no adapter (J-Link BASE 20-pin):** 1 VTref → 3V3 header ·
  7 SWDIO → J11.2 · 9 SWCLK → J11.4 · 15 RESET → RST header · 4 GND → GND header ·
  (13 SWO → J11.6, optional). Pin 19 (5V-Supply) is left unconnected.
  Diagram: [`docs/wiring.html`](docs/wiring.html), published at https://claude.ai/artifact/244kEtWoGoCCKFDKcuLoxq
- **VTref:** power the board from USB. The J-Link only senses target voltage and does not
  power the board.

### 3.2 Flash map

| Range | Contents |
|---|---|
| `0x00000–0x0BFFF` | Ambiq secure bootloader (ROM-ish, don't touch) |
| `0x0C000–0x0FFFF` | SparkFun SVL |
| `0x10000–…` | **Application (Zephyr `code-partition`)** |

**Rule: always link at `0x10000`, even when flashing with the J-Link.** Then J-Link
(`west flash` / `west debug` at the offset) and USB SVL (`flash.py`) are interchangeable, and
the SVL is never erased. This makes USB recovery always available.

### 3.3 USB (SVL)

`scripts/flash.py` (to write): `zephyr.bin` → pad to a multiple of 4 → SparkFun
`artemis_svl.py` at 921600 baud. The CH340E's RTS line resets the board into the SVL.

## 4. Hive integration

No WiFi or Thread, so `craw_hive` (mDNS + HMAC over WiFi) and the Hanasu Thread mesh are out.

1. **v1 — BLE peripheral speaking an HCP subset.** `WHOAMI`, `SYSINFO`, plus turret verbs
   (§6). Reachable from `magnet_app` and `hcp.py` the same way Hanasu's BLE companion path is.
2. **Fallback — UART sidecar to an ESP32** (the Milk-V Duo + M5Stamp C3U pattern) if the
   Zephyr Apollo3 BLE stack misbehaves.

The Forth engine and signed role bundles are ESP-IDF-bound. For v1 the turret is a fixed-firmware
role with fixed verbs. A Zephyr port of the engine is deferred.

## 5. Bring-up phases

| # | Goal | Gate |
|---|---|---|
| 0 | Bench: stock SVL upload of a SparkFun example over USB-C. Solder the SWD header. Read the target with J-Link Commander (`connect`, device `AMA3B1KK-KBR`). | Board alive on both paths |
| 1 ✅ | Toolchain: `west` workspace pinned to Zephyr v4.4.2, Zephyr SDK 1.0.1, `hal_ambiq` (+ `mbedtls`, `tf-psa-crypto` for BLE). Build `hello_world` for unmodified `apollo3_evb`. | Build passes (proves the toolchain, not the board). Done 2026-10-18. |
| 2 | Board `boards/sparkfun/artemis_nano/`, derived from `apollo3_evb`. Code partition `0x10000`, UART0 console on the CH340E pins, user LED. `flash.py` for SVL. `west flash` via J-Link at the offset. | Blinky + console, flashed **both** ways |
| 3 | **Servo** (§6): `ambiq,ctimer-pwm` → Zephyr `samples/basic/servo_motor` adapted to the FS0307 range | Servo sweeps the full range, no jitter |
| 4 | BLE: Ambiq HCI driver + Zephyr `peripheral` sample | Phone (nRF Connect / `magnet_app`) connects |
| 5 | Hive glue: HCP subset over a BLE GATT service, plus the turret verbs | `AIM 30` from `hcp.py`/app moves the servo |

Verified from the schematic (2026-10-18): UART0 = pads 48/49, blue LED = pad 19, SWD pinout.
Still unverified: the CTIMER segment for pad 39, and the first Zephyr release with working Apollo3 BLE.

## 6. Actuator — FEETECH FS0307 servo

| Spec | Value | Source |
|---|---|---|
| Type | Analog **positional** sub-micro servo, 4.4 g | Pololu |
| Travel | ~120° on 900–2100 µs. ~180° on 650–2500 µs (Pololu tests, not FEETECH-rated, varies by unit). | Pololu |
| Signal | Standard RC servo PWM, 50 Hz frame. **Works with 3.3 V signals.** | Pololu |
| Supply | Rated at 6 V. Expect 4.8–6 V. Stall current not published. Budget ~0.5 A per servo. | Pololu / assumption |
| Torque / speed | 0.6 kg·cm, 0.09 s/60° @ 6 V | Pololu |
| Connector | JR/Futaba 3-pin: brown/black = GND, red = V+, orange/yellow/white = signal | Pololu FAQ |
| Caution | Stalling or back-driving can damage it. Keep the turret load light and don't hold it against a hard stop. | Pololu |

### 6.1 Wiring

```
 5 V supply (USB 5V breakout or 4×AA NiMH ≈ 4.8 V)
   (+) ──────────────────────────┬───────── servo RED
                                 ═ 470 µF electrolytic (across servo V+/GND)
   (–) ──────────────┬───────────┴───────── servo BROWN
                     │
 Artemis Nano  GND ──┘      (common ground — required)
 Artemis Nano  Dx  ──────────────────────── servo ORANGE (signal, 3.3 V)
```

- **Do not power the servo from the Nano's 3V3 pin.** That's a 600 mA AP2112 regulator shared
  with the Apollo3, and a servo stall will brown out the MCU.
- Signal pin: **D9 = Artemis pad 39** (PWM-capable per the schematic). Confirm its CTIMER segment
  in phase 2. A second servo (tilt) only needs one more `~` pad (e.g. D11 = pad 5).
- Other pads from the schematic: blue LED = pad 19, UART0 TX/RX → CH340E = pads 48/49.

### 6.2 Firmware sketch

- Devicetree: a `pwm-servo`-style node on `ambiq,ctimer-pwm`, period 20 ms, with
  `min-pulse = 900 µs` and `max-pulse = 2100 µs` as the safe rated range. Widen to the 650–2500 µs
  range only after measuring each unit's end stops.
- API: `turret_aim(deg)` with `deg ∈ [-60, +60]` mapped linearly onto the pulse range and clamped.
  A slew limit (°/tick) protects the gears and the power supply.
- Verbs (phase 5): `AIM <deg>`, `CENTER`, `SWEEP <lo> <hi> <ms>`, `POS`, `RELAX` (stop pulses
  so the servo stops holding position).
- Fallback if CTIMER PWM won't come up: a GPIO pulse timed by a `counter`/`k_timer`. Jitter will be
  worse but fine for a prototype.

### 6.3 Parked: ULN2003 + 28BYJ-48 stepper

Set aside 2026-10-18 in favor of the servo. For later: ULN2003 inputs accept 3.3 V logic.
Motor needs its own 5 V and a common ground. 4 GPIOs per motor.
