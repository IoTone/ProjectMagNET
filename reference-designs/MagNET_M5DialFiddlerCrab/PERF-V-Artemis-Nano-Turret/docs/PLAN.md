# PERF-V-Artemis-Nano-Turret — Plan

Status (2026-10-30): **T5a done in simulation. No hardware wired, nothing
synthesized, no firmware built.** `rtl/` holds `step_gen`, `phase_seq`,
`motion_axis` and `sweep` with three passing testbenches (`sim/run.sh`) — the
only part of this project that builds on macOS. Everything else is still
planning. Carries forward the Multisense node (`ArtemisNano_Hive_Multisense`,
fw `0.5.0-m4`, itself never compiled) and the turret parked in that project's
Appendix A.

## 1. Goal

A two-board hive member that **aims a stepper at something it senses** — light
first, a person second, and whatever a later module can see or hear after that:

- **Perf-V** (Xilinx Artix-7 **XC7A35T-1FTG256C**, PerfXLab / Seeed `102110277`) — the motion
  controller. Stepper drive, acceleration, position, limits. No radio, no network.
  **Discontinued; the board on the bench is the only one there will be.**
- **Artemis Nano** (Ambiq Apollo3 Blue, Zephyr) — the senses and the only radio.
  Reduced to **two** sensors: colour sensor and person detector.
  Bridges the hive to the Perf-V.

The Artemis is the hive member; the Perf-V is an actuator behind it. Everything
the hive or `magnet_app` sees goes through the Artemis.

### 1.1 Scope change from Multisense

| Part | Multisense | Turret | Why |
|---|---|---|---|
| LC709203F fuel gauge `0x0B` | ✅ | ❌ **drop** (2026-10-29) | No battery fitted, so there is nothing to gauge. See D8 |
| OPT4048 colour `0x44` | ✅ | ✅ keep | Ambient gate + scene CCT |
| Person Sensor `0x62` | driver written, untested | ✅ keep — **now the primary sensor** | It is what the turret aims at |
| BNO085 IMU `0x4A` | ✅ | ❌ **drop** | Not needed when position comes from the step counter |
| RV-1805 RTC `0x69` | ✅ | ❌ drop | Mains-powered; hive mesh time is enough |

Dropping the BNO085 removes **11.00 mA** — the load README §4.3 identified as 75%
of the Multisense budget. That is a happy accident of this scope change, not a
reason for it.

### 1.2 Goals

| # | Goal | Where it is worked out | Status |
|---|---|---|---|
| **G1** | **Aim where it is told.** `T.MOVE <angle>` from the phone lands within a step, with a ramp and real limits | §5, T0–T7 | planned |
| **G2** | **Orient toward light by sweeping.** Walk the pan arc, sample the OPT4048 at known bearings, turn to the brightest one | §4.6, T8a | planned — **the primary v1 behaviour** |
| **G3** | **Choose *which* light.** Use CIE xy / CCT, not only lux, so a 2700 K lamp can be preferred over a 6500 K window | §4.6.3, T8a | planned, no extra RTL |
| **G4** | **Track a face.** Closed loop on the Person Sensor inside its ~55° field of view | §4.4, T8b | planned |
| **G5** | **Take a better tracker later.** A camera or mic module drops in as a new *bearing provider* without touching the motion RTL, the link, or the app | §4.7 | **designed for, not built** |

### 1.3 The one idea that makes G2–G5 a single feature

The sensors fall into exactly two kinds, and the difference is not what they
measure — it is whether a *single reading* contains a direction.

- **Scalar, non-directional** — lux, colour, RSSI, one microphone. A reading says
  "this much, from everywhere." The bearing comes from the **motion**: sweep, and
  the turret itself becomes the sensor's aperture. This is G2/G3.
- **Framed, directional** — Person Sensor, camera, mic array. Every reading
  already carries a bearing, within a limited field of view, with no motion at
  all. This is G4/G5.

They compose in one order: **scan to acquire, frame to track.** A sweep finds a
coarse bearing over the whole arc; a framed sensor then closes the loop inside its
narrow FOV. That is the whole behaviour, and it is why G5 is cheap — see §4.7.

Above the UART, everything is the same object:

```
bearing_estimate {  bearing_deg,  confidence,  age_ms,  source  }
```

The Perf-V never learns which sensor produced it. That is the line that keeps the
RTL stable while the sensing changes.

## 2. Decisions (settled)

| # | Decision | Why |
|---|---|---|
| **D1** | **The Artemis is an HCP bridge.** Verbs prefixed `T.` are proxied over the UART to the Perf-V; everything else is answered locally. | Hanasu already speaks HCP, the Artemis already has a parser (`hive_ble.c`), and `magnet_app` already speaks it. The turret becomes reachable from the phone with **zero new protocol**. HCP's `!` async lines map exactly onto motion-complete events. |
| **D2** | **Link is UART, 3.3 V, direct — no level shifting.** | Perf-V banks 14/15/34 run at 3.3 V (user manual §2.3.2); the Apollo3 is a 3.3 V part. Confirmed from the manual, not assumed. |
| **D3** | **The wire protocol between the two boards is binary, not text** (8-byte frame: sync, cmd, 4-byte arg, CRC8). HCP's text form stops at the Artemis. | A string parser in fabric is the worst part of a pure-RTL design. The Artemis is already parsing text; let it translate. **This reverses for Option B** (§5) — with a soft core, text HCP end-to-end is natural. |
| **D4** | **Step generation stays in RTL in both options.** | Bit-banging steps from a soft core throws away the only reason to use an FPGA. Option B adds a CPU *above* the peripheral, it does not replace it. |
| **D5** | **Option A (pure RTL) is built first, in both plans.** It is a prerequisite for Option B, not an alternative to it. | See §5. Makes the "two options" a sequence, not a fork. |
| **D6** | **Perf-V 5 V powers the Artemis** through the Nano's `VIN` (same path USB uses: 5 V → Schottky → `VIN` → AP2112). | One supply, guaranteed common ground, and it sidesteps the USB-to-battery brownout in Multisense §4.2 entirely — there is no changeover. |
| **D7** | **The motor gets its own 5 V supply**, common ground with the Perf-V. Not the board's `5V_IN` pin. | 28BYJ-48 draws ~300 mA peak with switching transients; the Perf-V's 5 V rail feeds four TPS54331 bucks and the FPGA. Appendix A of Multisense already said this. |
| **D8** | **No battery and no fuel gauge. The Artemis is powered entirely from the Perf-V.** | **Corrected 2026-10-29.** An earlier draft kept the cell as a UPS so the gauge would have something to read — reasoning backwards from a sensor nobody needed. A mains-fed turret has no use for either. Two consequences worth having: the Person Sensor can stay in its **default continuous mode** at ~7 Hz, so the standby / single-shot path leaves the design entirely; and the Multisense power budget (§4.3 there) now has no bearing on this project at all. |
| **D9** | **Hardware e-stop.** A Perf-V user button cuts the step train in one 50 MHz clock, in fabric, with no CPU in the path. | A real safety property an MCU design cannot match. Cheap here: it is one AND gate. |
| **D10** | **Microstepping is out of scope.** | A ULN2003 is a saturated Darlington switch with no current control. Full-step (4-phase) and half-step (8-phase) only. PWM'ing the inputs is noted as a speculative stretch in §4.3, not a planned feature. |
| **D11** | **Sweeping is how a non-directional sensor gets a bearing.** The turret is the aperture; the scan is the measurement. | A single OPT4048 cannot be aimed, so §1.3. This makes G2 a *motion* feature, which is the half of the design the FPGA is actually good at (§4.6.6). |
| **D12** | **The OPT4048 gets a baffle, and it is not optional.** A tube over the sensor, built and *measured* before any peak-finding code is written. | A bare OPT4048 sees ~±80° cosine-weighted. Swept bare, a 180° arc changes the reading by a few percent — there is no peak to find. **The baffle, not the stepper, sets the angular resolution of G2.** See §4.6.2 and R10. |
| **D13** | **Scan-and-dwell for v1; on-the-fly later.** Step, stop, **dwell two conversions**, read, repeat. | Removes all skew between position and reading with zero RTL. **Revised 2026-10-30 from the T5a simulation**, which changed two numbers. (a) The dwell is **220 ms, not 100 ms**: one conversion time only guarantees that *a* conversion finished, not that the one you read *began* after the axis was stationary (§4.6.6). (b) On-the-fly is worth **~3.3×**, not the modest win implied here — a measured 180° scan is 8.35 s, of which only 4.18 s is dwell; the other 4.17 s is 19 short moves that never reach `vmax`, against 2.48 s for the same arc travelled once. Still not v1, but now a known price. |
| **D14** | **One bearing-provider interface** (§1.3), and converting the provider's own units to degrees is the *provider's* job. | Only the provider knows its own FOV. Hold that line and adding a camera touches one arbitration table on the Artemis — not the RTL, not the link, not `magnet_app`. Break it and every new sensor is a protocol change. |

## 3. Hardware

### 3.1 Confirmed from the Perf-V user manual

| Item | Value | Source |
|---|---|---|
| FPGA | **XC7A35T-1FTG256C** — 33,280 logic cells, 90 DSP, 41,600 CLBs, 1,800 Kb BRAM, 5 CMTs. Confirmed 2026-10-29 (O1 closed) | perfv.org §Main Chips; Seeed SKU `102110277` |
| I/O banks on headers | Bank 14, 15, 34 — **all 3.3 V** | §2.3.2 |
| Clock | 50 MHz active oscillator → bank 14 global clock pin **N14** (`IO_L12P_T1_MRCC_14`) | §2.4 |
| P2 (Arduino) | D0–D13, A0–A5, plus SPI (`CK_SS` R7 / `CK_MOSI` R6 / `CK_MISO` T5 / `CK_SCK` R5) and I²C (`SDA` T8, `SCL` T7) | §2.2.2 |
| JP1 (PMOD) | 2×6, **8 GPIO** + 2×GND + 2×3V3 | §2.2.3 |
| P1 (high speed) | 17 differential pairs, XADC, 5V/3V3/1V8 | §2.2.1 |
| Power in | **5 V**, via power key and fuse → TPS54331 ×4 → 3.3 V / 1.8 V / 1.5 V / 1.0 V, TPS51200 → VTT 0.75 V | §2.3.1 |
| On-board UI | 3 RGB LEDs, 4 red LEDs, 4 soft-touch buttons, 4 toggle switches, 1 reset | §2.6 |
| Memory | 256 MB DDR3 (MT41J128M16JT), 8 MB FPGA flash + 8 MB RISC-V flash (both N25Q064A) | §Main Chips |

**Not stated anywhere in the manual or on perfv.org: board power draw.** Estimated
**1.5–2.5 W (300–500 mA at 5 V)** from the Artix-7 plus four always-on bucks plus a
powered-but-unused DDR3. **Unmeasured — measure it at T1.** The consequence is
settled regardless of the exact figure: **the turret is mains-powered.** Multisense
§4.3's 85-day target does not apply to this project and should not be quoted at it.

### 3.2 Motor — ELEGOO ULN2003 + 28BYJ-48

| Property | Value | Consequence |
|---|---|---|
| Type | Unipolar, 5 V, 4-phase | FPGA drives IN1–IN4 directly; no STEP/DIR |
| Stride | 5.625° before the gearbox | 64 half-steps per rotor revolution |
| Gear ratio | **1:63.68395**, not 1:64 | **4075.77 half-steps/rev, not 4096.** Using 4096 accumulates ~0.5% error — about **1.8° per revolution**. Use the true ratio, or better, home against a limit switch every revolution |
| Resolution | 360 / 4076 = **0.088°** per half-step | Far finer than the turret needs |
| Backlash | ~1–2° typical in the gear train | **This, not step resolution, is the accuracy limit.** An output-shaft encoder is the only way past it |
| Speed | ~10–15 RPM max | Slow. A 180° slew takes ~2 s. Fine for tracking a person |
| Torque | ~34 mN·m | Light payload only — a sensor head, not a lamp |
| Current | ~300 mA peak, all phases | Own supply (D7) |

**ULN2003 electrical notes:**
- Inputs have a ~2.7 kΩ series base resistor, so the FPGA sources ~1 mA per input.
  Well inside Artix-7 3.3 V LVCMOS drive. 3.3 V drive works, with less margin than 5 V.
- **The inputs have no internal pull-down, and FPGA pins are high-Z until
  `FPGA_DONE` asserts.** Floating Darlington inputs can partially conduct, so the
  motor may energize and heat during configuration. **Fit 10 kΩ pull-downs on
  IN1–IN4.** This is a hazard an MCU design does not have and it must be designed
  in, not discovered. See R3.
- The board's 4 phase LEDs are a free logic analyser during bring-up.

### 3.3 Proposed pin map

Perf-V — **all pin names from the manual's tables; the UART assignment is Arduino
convention and must be confirmed against the schematic (O2):**

| Signal | Connector | Pin | FPGA pin |
|---|---|---|---|
| UART RX (from Artemis TX) | P2 | D0 | N6 |
| UART TX (to Artemis RX) | P2 | D1 | M6 |
| Pan IN1–IN4 | JP1 | 1, 2, 3, 4 | E12, E13, E11, D11 |
| Pan limit CW / CCW | JP1 | 7, 8 | D14, D15 |
| Tilt IN1–IN4 (axis 2, later) | P2 | D4, D5, D6, D7 | P13, N13, R11, R10 |
| Ground to ULN2003 + Artemis | JP1 | 5, 11 | — |

Deliberately avoiding P2 D2/D3 — they are the same FPGA pins as `SDA`/`SCL`
(T8/T7), and keeping I²C free costs nothing. Limit switches use Vivado
`PULLUP TRUE` and switch to ground, so they need no external parts.

**Artemis UART1 pads: not yet chosen (O3).** Only UART0 is in use (CH340E).
The Apollo3 has a second UART but its TX/RX can only land on specific pads;
pick from the Apollo3 pin-mux table and the board DTS before wiring.

Series 100–330 Ω on the UART pair, cheap insurance against both ends driving.

## 4. What this design makes possible

The question was what the FPGA buys. Split honestly.

### 4.1 Real wins on this hardware

1. **Jitter-free step timing.** A phase accumulator clocked at 50 MHz places
   every edge to ±20 ns regardless of load. An MCU's step timing moves with
   interrupt load, and on a stepper that shows up as audible noise and lost steps.
2. **Hardware acceleration ramp.** Trapezoidal profile computed in logic, one
   update per step, no CPU. **The 28BYJ-48 needs this** — started at speed it
   simply stalls, and stalls are silent on an open-loop drive.
3. **Two axes, genuinely synchronized.** Instantiate the module twice; they
   share a clock, so a coordinated pan+tilt move needs no software interpolation.
4. **Limit and e-stop in one clock.** The step train is gated in fabric. No
   interrupt latency, no firmware that can be wedged.
5. **Exact position, free.** A counter on the step output is the position,
   by construction.
6. **A step-accurate trace in BRAM.** 1800 Kb is a lot of timestamped step and
   limit history for diagnosing a stall after the fact.
7. **A real front panel for free.** 4 toggle switches, 4 buttons, 7 LEDs already
   on the board: jog, enable, home, e-stop, and per-axis state — with no
   enclosure and no app.
8. **Quadrature decode** if an encoder is fitted later. Given 1–2° of backlash,
   an output-shaft encoder is the only route to real accuracy, and decoding it
   in fabric is a dozen lines.

### 4.2 What the FPGA does *not* buy

- **Any connectivity.** No radio, no Ethernet, no USB host. Everything goes
  through the Artemis. This is the single biggest constraint on the design.
- **Loop bandwidth.** The Person Sensor runs at ~7 Hz; that caps the tracking
  loop, not the controller. The FPGA's determinism buys smoothness *within* a
  move, not a faster loop. Do not oversell this.
- **Low power.** §3.1. Wall-powered, and it is roughly 2000× the Artemis's
  M5 target.
- **Fast iteration.** Vivado synthesis is minutes; a firmware rebuild is seconds.
- **Microstepping.** D10.

### 4.3 Speculative, not planned

PWM'ing the ULN2003 inputs from a sine LUT to approximate current control. An
FPGA is well suited to four independent PWMs, and people do get smoother unipolar
motion this way. But there is no current feedback, winding inductance limits how
much it helps, and gearbox backlash probably swamps the benefit. **Try it only
after T4 works, and only if motion is visibly rough.**

### 4.4 The capability

Closed-loop person tracking:

```
Person Sensor 0x62 ──I2C──► Artemis (Zephyr)
   up to 4 faces: box, confidence, is_facing, recognised ID
        │  error = face_centre_x − 128
        ▼
   binary frame ──UART 115200──► Perf-V
                                   │ ramp + step_gen (RTL)
                                   ▼
                              ULN2003 ──► 28BYJ-48
                                   │
   ◄──── !MOVED <pos> / !LIMIT ────┘

   Artemis ──BLE/HCP──► magnet_app, hive
```

The colour sensor is also a **gate** on this loop: the Person Sensor needs light
to see faces at all, so below a lux threshold the turret should stop hunting and
say so rather than hunt in the dark. Scene CCT is worth publishing either way —
it tells the hive whether this room is on daylight or tungsten.

That gate is the *smaller* of the colour sensor's two jobs. The larger one is
G2/G3, and it has its own section.

### 4.5 The mechanical decision nobody should skip

**Where does the Person Sensor sit?** This decides the build, and it is easy to
leave until too late.

- **(i) Sensor rides the turret.** True tracking: drive the error to zero,
  unlimited rotation. But the Qwiic chain crosses the rotating axis — needs a
  cable routed through the axis with a turn limit, or a slip ring. The limit
  switches in §3.3 exist for exactly this.
- **(ii) Sensor fixed, turret aims within its field of view.** No cable problem
  at all, but it needs a calibration mapping face position → turret angle, and
  the turret can only ever aim within the sensor's ~55° half-angle.

**(ii) is the right first build** — it gets the whole loop working with no
mechanical risk — and (i) is the better turret. Plan for (ii) at T8 and treat (i)
as a later mechanical revision.

### 4.6 Orienting toward light (G2, G3)

#### 4.6.1 A sweep is the only option, and that is fine

The OPT4048 is one photodiode cluster behind a diffuser with a roughly
cosine-weighted hemispherical response. **There is no direction in a single
reading** — only "this much light arrived." So the bearing has to come from
somewhere else, and the only thing on hand is the turret's own position (D11).

This is not a workaround. A swept scalar sensor is a real instrument: it trades
time for angle, and the thing it produces — a curve of signal against bearing —
is strictly more informative than a single framed reading, because it covers the
*whole arc* instead of a 55° window.

#### 4.6.2 The baffle is the sensor (D12)

Swept bare, the OPT4048's response is so broad that a 180° rotation changes the
reading by a few percent. There is no peak; the scan is flat and the argmax is
noise. A tube over the sensor fixes this, and the tube's geometry — not the
stepper's 0.088° — is what sets the angular resolution of the whole feature:

| Bore × depth | Rough half-angle | Samples for 180° at Nyquist |
|---|---|---|
| 6 mm × 6 mm | ~±27° | ~7 |
| 6 mm × 15 mm | ~±11° | ~16 |
| 4 mm × 20 mm | ~±6° | ~30 |
| 4 mm × 40 mm | ~±3° | ~60 |

Narrower costs light (less signal, longer conversions) and costs time (more
samples). **~±10° is the sweet spot for v1**: a 180° coarse sweep is ~16 samples,
and 10° is far finer than anything downstream needs. Matte black inside, or the
walls reflect and widen it back out. Black heatshrink over a brass tube is enough;
a printed snoot is nicer.

**Measure it before trusting it.** Put a lamp at a known bearing, run one sweep,
and plot the curve. If it has a single clean peak near the lamp, the feature
works. If it is flat or double-humped, no amount of code helps. This is T8a's
first task, ahead of the peak finder (R10).

#### 4.6.3 What gets maximized is a choice (G3)

The scan machinery does not care what scalar it is maximizing, so make the metric
a named function and the interesting behaviours come free:

| Metric | Scalar | Behaviour |
|---|---|---|
| `lux` | `Y` | Turn to the brightest thing. The default |
| `match` | `Y / (1 + k·d²)`, `d` = CIE xy distance from a target chromaticity | Turn to the brightest thing *of a chosen colour*. Prefers a 2700 K lamp over a 6500 K window — the demo worth showing |
| `delta` | `Y − Y_background(θ)` against a stored sweep | Turn to whatever **changed**. Costs one baseline scan; makes "a light just came on" a bearing |

All three come out of the existing driver: `opt4048_read()` already returns CIE
x, y, lux and CCT. `match` and `delta` are arithmetic on readings the node is
already taking, with no new hardware and no RTL change. `T.SEEK <metric>` selects.

#### 4.6.4 Finding the peak without believing the noise

Taking the argmax of a raw scan is the wrong answer — it is one sample, with one
sample's noise, quantized to the sweep step. The procedure:

1. **Coarse sweep** the full arc at step = baffle half-angle (Nyquist on the
   aperture, §4.6.2). ~16 samples for ±10° over 180°.
2. **Decide whether there is a source at all.** If `max − median` is under a few
   times the sample-to-sample noise, report `!SEEK nosource` and **do not move.**
   A flat scan means diffuse light, and aiming at the argmax of flat noise is the
   failure mode that reads as a possessed turret.
3. **Parabolic interpolation** on the three samples bracketing the max — a
   three-point fit gives a bearing well inside one sweep step, for nine lines of
   arithmetic. This is where the resolution actually comes from.
4. **Optional fine sweep**, ±2 coarse steps at a quarter of the step, and take a
   centroid over samples above half-max. Better in a cluttered room; skippable.
5. **Move, approaching from one direction** — backlash is 1–2° (R5) and consistent
   approach direction is free.

#### 4.6.5 Not hunting

A seeker that re-aims on every scan oscillates. Four cheap brakes:

- **Dead-band.** Move only if the new peak is more than ~1.5× the backlash away
  *and* more than ~10% stronger than the current bearing's reading.
- **Rate limit.** One sweep per N seconds, floor on N.
- **Event-driven re-scan instead of polling.** The OPT4048's `INT` pin can fire on
  a lux threshold — the Multisense pin map already routes it (D7 / pad 42) and
  never used it. Here it is exactly right: *the light changed, go look again.*
  This is the single best reason to wire that pin.
- **`T.SEEK OFF`.** Manual aim always wins; G1 outranks G2.

#### 4.6.6 What the FPGA contributes here

The sweep is the part of this design the FPGA is genuinely suited to, because it
is motion with exact position:

- **`SWEEP` as a hardware primitive** (§5.0). The Artemis writes start, end, step
  and dwell *once*; the fabric walks the arc and raises a sample strobe at each
  station. The Artemis reads lux on the strobe and never commands a step. That
  removes 16–60 UART round-trips per scan and all their latency variance. Built
  and simulated: `rtl/sweep.v`, 150 lines including comments.
- **The order is arrive → dwell → strobe, and the dwell is two conversions.**
  The OPT4048 integrates continuously, so a reading taken the instant the motor
  stops is smeared across the arc just crossed. One conversion time is the
  tempting number and it is wrong — it only guarantees that *a* conversion
  finished, not that the one being read *began* after the axis was stationary.
  Hence `dwell_ms ≥ 2 × conversion_ms`: **220 ms**. With the OPT4048 `INT` line
  wired to the FPGA this becomes "strobe on the second conversion-done after
  arrival" and the dwell disappears — the `POS_LATCH` path of D13.
- **Exact bearings for free.** `POS` is a counter, not an estimate, so every sample
  is tagged with its true bearing — no dead reckoning, no drift, nothing to
  calibrate. A microstepping driver with no position feedback could not do this.
- **`POS_LATCH` for the on-the-fly sweep** (D13). Sweep continuously and latch
  `POS` on the OPT4048's conversion-done, and the scan takes as long as the travel
  rather than travel + N × 100 ms. The integration smears over the arc crossed
  during 100 ms, which is a box filter — a feature, as long as the latched
  position is the midpoint. Not v1; the register exists so it does not become a
  protocol change later.

### 4.7 Adding a tracker later (G5)

G5 is a design constraint on v1, not a phase. The constraint is D14: a provider
publishes `bearing_deg, confidence, age_ms` **in the turret's own frame**, and
converting its own pixels, samples or lobes into degrees is its job, because only
it knows its FOV. Hold that and a new sensor touches one arbitration table.

| Provider | Bearing from | Lives on | The honest cost |
|---|---|---|---|
| **Person Sensor** `0x62` | face box centre, ~55° FOV, ~7 Hz | Artemis, I2C | Already fitted. FOV-limited, and needs light — hence the §4.4 gate |
| **One mic** (PDM/I2S) | **scan only** — level vs bearing, exactly like light | Artemis | Cheap to fit, poor as a seeker: a mic's pattern is far broader than a photodiode's and the acoustic equivalent of the baffle is a horn or parabola, which is bulky. Expect worse angular resolution than the light scan, not better |
| **Two mics, TDOA** | cross-correlation of the pair | **Perf-V** | The one case where this FPGA is the *right* chip rather than overkill: continuous cross-correlation at MHz rates is what fabric does. A ~100 mm baseline gives a usable bearing; front/back is ambiguous without a baffle or a third mic. The strongest technical argument for the whole two-board design |
| **Camera** | blob / centroid / face box | **not the Artemis** | The Apollo3 has no camera port and ~384 KB of RAM. Two real paths: (a) a DVP sensor straight into the Perf-V — 320×240 mono is ~614 kb and fits the 35T's ~1.8 Mb of BRAM, but it is weeks of work; (b) a third board (ESP32-CAM class) that does its own vision and publishes a bearing over HCP — an afternoon, and it makes the turret a three-node assembly. Judgement, not a technical toss-up (O9) |

**Arbitration**, when more than one provider has an opinion:

1. Framed providers outrank scan providers whenever a framed one is above its
   confidence floor. *Scan to acquire, frame to track* (§1.3) is the rule.
2. Among framed providers, highest confidence; ties break toward the narrower FOV.
3. A provider whose `age_ms` exceeds twice its own period is dropped, not trusted
   stale. A camera that stops talking must not freeze the turret pointing at a
   ghost.
4. G1 outranks everything. An explicit `T.MOVE` cancels seeking.

No step of that is in the RTL, the wire protocol, or the app. That is the point.

## 5. The two FPGA options

The user asked for both. They are **stages, not alternatives** (D5): Option B is
Option A plus a CPU that writes the same registers.

### 5.0 The register map — shared by both

| Offset | Register | Access | Meaning |
|---|---|---|---|
| `0x00` | `CTRL` | W | enable, dir, go, stop, home |
| `0x04` | `TARGET` | W | target position, signed, half-steps |
| `0x08` | `POS` | R | current position |
| `0x0C` | `VMAX` | W | steps/s |
| `0x10` | `ACCEL` | W | steps/s² |
| `0x14` | `STATUS` | R | moving, at_target, limit_hit, estop, fault |
| `0x18` | `LIMITS` | W | per-limit enable and polarity |
| `0x1C` | `SWEEP_CFG` | W | start, end, step (half-steps), dwell (ms) — §4.6.6 |
| `0x20` | `SWEEP_CTL` | W | go, abort; progress in `STATUS.sweeping`, station index readable |
| `0x24` | `POS_LATCH` | R | `POS` captured on the last sample strobe. Reserved for the on-the-fly sweep (D13); reads as `POS` in v1 |

Option A drives these from a binary-frame decoder. Option B drives them from a
CPU bus. **The motion RTL is identical.**

### 5.1 Option A — pure RTL (recommended first)

Modules: `uart_rx`, `uart_tx`, `frame_decode`, `regfile`, `ramp`, `step_gen`,
`phase_seq` (the 4-phase/8-phase table), `limits`, `estop`, `pos_counter`.

- **Size:** ~600–900 lines of Verilog including testbenches.
- **Resource:** well under 5% of even the 35T. Capacity is a non-issue.
- **Tools:** **Vivado ML Standard only.** Verified: every Artix-7 part from
  XC7A12T to XC7A200T is in the free Standard Edition device table (AMD UG973),
  and no licence has been needed since Vivado 2016.x. No RISC-V SDK, no licence
  code by email.
- **Risk:** low, and entirely in the toolchain install.

### 5.2 Option B — RISC-V soft core (later)

perfv.org lists four cores: **X-Core** (PerfXLab), **"Hummingbird E200"**,
**Syntacore SCR1**, and **UCTechIP WH32 — 100T only**. Note the page says *E200*;
the well-known open core is *E203*, so the naming needs checking before anyone
clones a repo.

- **Gains:** text HCP parsed in C, so D3 reverses and the protocol is uniform
  end to end; trajectory queues in BRAM or DDR3; motion profiles in software;
  firmware iteration without re-synthesis; the possibility of running real MagNET
  node code on RISC-V.
- **Costs:** core bring-up, a second toolchain, the separate 8 MB RISC-V flash,
  CKLink/JTAG debug, and documentation that is thin, partly Chinese, and dated
  2019. **The Perf-V IDE requires a licence code requested by email** — a 2019
  address, so assume it may not answer.
- **Gate:** do not start Option B until T4 (motor moving under RTL control) is
  done. If the 2019 tooling turns out to be dead, Option A still ships a working
  turret and nothing is lost.

## 6. Phases

Each stage ends with something observable on hardware. Nothing proceeds on a
stage that has not been seen working.

| # | Stage | Done when |
|---|---|---|
| **T0** | Settle the open items in §8. Confirm the FPGA part, the Perf-V UART pins against the schematic, the Artemis UART1 pads | O1–O3 closed |
| **T1** | **Vivado hello.** Project for the right part, 50 MHz on N14, blink a user LED, read a toggle switch. Flash it. **Measure board current.** | LED blinks from flash after a power cycle; a current figure exists |
| **T2** | **UART loopback in RTL.** 115200 8N1 RX+TX, echo, tested from a USB-serial on the bench — not yet the Artemis | Characters round-trip; no framing errors over a minute |
| **T3** | **Step generator + ramp, simulated then on LEDs.** Testbench first; then drive the 4 red LEDs slowly enough to read the phase pattern by eye | Phase sequence correct in sim and visibly correct on the LEDs; ramp observable |
| **T4** | **Motor moves.** ULN2003 wired with its pull-downs (§3.2), own 5 V supply. Limit switches and e-stop in the path | Commanded revolution lands within a step; e-stop halts mid-move; a limit stops it |
| **T5** | **Binary frame decoder + register file.** Drive it from a host script over USB-serial | Position, speed and accel settable from the bench; `STATUS` reads back |
| **T5a** | ✅ **done in simulation 2026-10-30.** `rtl/sweep.v` + `motion_axis.v` + `step_gen.v` + `phase_seq.v`, three passing testbenches (`sim/run.sh`, Icarus, runs on the Mac). Still to do on hardware: watch the strobe on a scope or an LED | **Met in sim.** A full arc runs from one write; every station strobes at its exact `POS` with the axis provably stationary; a non-dividing step stops inside the arc; a zero step is refused; abort ramps down, stays down and re-arms. Not synthesized, not on a board, no motor |
| **T6** | **Artemis sensor trim.** Strip BNO085 + RV-1805; Person Sensor on the live path. *This is also where Multisense's unbuilt `0.5.0-m4` finally gets compiled* | `CAPS` lists exactly the two sensors; `SENSE` returns face count; the Person Sensor driver meets hardware for the first time |
| **T7** | **The bridge.** Artemis UART1 up, `T.` prefix proxied, `!` events forwarded to BLE | `T.POS` answered from the phone, through two hops |
| **T8a** | **Light seeking (G2, G3).** Baffle built and its angular response **measured first** (§4.6.2), then the peak finder, then the `match` and `delta` metrics | A lamp at an unknown bearing is found within ~2× the baffle half-angle; a flat scan reports `nosource` and does not move; `match` picks a 2700 K lamp over a window |
| **T8b** | **Face tracking (G4)**, architecture (ii) of §4.5 | Turret follows a face across the sensor's field of view; stops hunting below the lux gate; arbitration (§4.7) hands control from scan to frame and back |
| **T9** | Role + `magnet_app` | Turret visible and drivable from the app |
| **T-RV** | Option B, gated on T4 | A soft core writes the §5.0 registers |

## 7. Risks

| # | Risk | Mitigation |
|---|---|---|
| **R1** | **Vivado is a 50+ GB install, Windows/Linux only.** It lands on the same Windows bench as the Zephyr workspace (`C:\zephyr-artemis`) — there is no macOS path for either | Accept. Both toolchains on the one bench machine. Nothing in this project can be built on the Mac |
| **R2** | **Perf-V tooling is 2019-era**; the IDE needs an emailed licence code and the forum may be dead | Option A needs none of it. Keep Vivado-only as the spine (D5) |
| **R2b** | **The board is discontinued and out of stock at Seeed** (confirmed 2026-10-29). No spare, no replacement path | Two consequences. (a) Treat the FPGA flash as precious: never leave the board mid-program, and archive a known-good bitstream in this repo from T1 onward. (b) It sharpens R2 — depend on Vivado and the schematic, not on PerfXLab's servers, email or forum, none of which are guaranteed to answer. Option B's reliance on an emailed licence code is now a real single point of failure |
| **R3** | **Motor energizes during FPGA configuration** — pins float until `FPGA_DONE` | 10 kΩ pull-downs on IN1–IN4, designed in at T4, not retrofitted. Verify by watching the ULN2003's own phase LEDs through a power cycle |
| **R4** | **4096 vs 4075.77 half-steps/rev** silently accumulates ~1.8°/rev | Use the true ratio; home against a limit switch. Do not let the round number into the RTL |
| **R5** | **Gearbox backlash (1–2°) dominates accuracy** | Accept for v1 — it is well within what person-tracking needs. Approach from one direction if it matters; an output-shaft encoder is the real fix |
| **R6** | **Nothing in the Artemis firmware has ever been compiled** (`0.5.0-m4`). That debt lands here | T6 is where it gets paid. Expect the first build to turn up real errors |
| **R7** | **The Person Sensor driver has never met hardware.** Two constants in it were marked unverified; D8 retires one of them, since the single-shot trigger register is no longer used | Before T6: power the sensor at 3.3 V and show it a face — its debug LED should go green with no firmware at all |
| **R8** | **Two-board ground and sequencing.** Two supplies now (Perf-V 5 V, motor 5 V) | D6/D7 fix the topology. Common ground at one point; bring the motor supply up before or with the logic. During bring-up, decide whether USB is also plugged in for the serial console — that puts a second 5 V source on the same node |
| **R9** | **Overselling the FPGA.** It is enormous overkill in compute for a 28BYJ-48 | §4.1/§4.2 state what it actually buys. The honest summary: the ramp generator and jitter-free timing are real; the accuracy limits are mechanical and the controller cannot fix them |
| **R10** | **No baffle, no light seeking** (D12). The cheapest part of the design is the one that decides whether G2 works at all, and it is the easiest to defer | Build the snoot and **plot its angular response against a lamp at a known bearing before writing any peak-finding code** (T8a). A flat curve is a mechanical problem and no algorithm fixes it |
| **R11** | **A sweep is slower than it looks, and conspicuous.** **Measured in the T5a simulation, correcting the estimate that was here**: a 180° coarse scan at ±10° spacing is 19 stations and **8.35 s**, not the ~2 s this row used to claim. Only half of that is dwell (4.18 s); the other half is 19 short moves that never reach `vmax`. The 28BYJ-48 is audible throughout | Three levers, in order of value. (a) **The dwell is the cheap half to remove** — `POS_LATCH` on-the-fly sweeping (D13) gets the whole scan to ~2.5 s. (b) Coarse-then-fine still helps, but budget ~8 s for a cold coarse scan, not 2. (c) Re-scan on the OPT4048 `INT` event rather than on a timer (§4.6.5), so an idle turret is a still, silent turret — this matters more now that a scan costs 8 s |
| **R12** | **The brightest thing is not the person.** G2 will cheerfully aim at a window, forever | Not a bug — but do not let it be described as tracking. §4.7's arbitration keeps the two behaviours separate and lets a framed provider take over. `delta` (§4.6.3) is the metric that ignores the window |
| **R13** | **The mechanical arc is undefined.** A sweep needs real arc limits; without them it winds a cable or hits a stop at speed. This is unspecified because §4.5 is unresolved | Home against a limit switch before the first sweep (R4's fix covers it), and clamp `SWEEP_CFG` in fabric to the homed arc rather than trusting the Artemis. O8 |

## 8. Open items

| # | Item | Needed by |
|---|---|---|
| ~~O1~~ | **Closed 2026-10-29. It is the XC7A35T.** Seeed SKU `102110277`, $79, tagged `XC7A35T`. The 126,800 CLB / 4,680 Kb figures that suggested a 100T belong to a *different* Seeed listing. No design change — the plan was already written against the 35T floor. Still worth a Vivado JTAG autodetect at T1 as a sanity check | ~~T1~~ done |
| **O2** | **Perf-V UART pins.** The manual says P2 carries "a set of UART ports" but assigns no pins. D0/D1 is Arduino convention. Confirm against `perfv_schematic.pdf` | T2 |
| **O3** | **Artemis UART1 pads.** Not chosen. Apollo3 UART1 TX/RX can only land on particular pads — check the Apollo3 pin-mux table against the board DTS | T7 |
| ~~O4~~ | **Closed 2026-10-29 by D8.** Person Sensor standby current is undocumented everywhere checked, but with the battery gone it no longer matters: the sensor runs continuous and its 45 mA comes off the Perf-V's 3 A rail | ~~T6~~ moot |
| **O5** | **Hive role number.** Multisense claimed Role 15 and is not yet in the top-level list; the Ear and Sentinel READMEs both claim Role 13. A turret is a new role — propose **Role 16: Turret** — but the existing collision should be resolved first | T9 |
| **O6** | **Second axis.** Pan only, or pan + tilt? §3.3 reserves pins for tilt. A second ULN2003 + 28BYJ-48 is cheap, and in RTL the second axis is one more instantiation | T3 |
| **O7** | **Baffle geometry.** Which half-angle (§4.6.2), and made how — heatshrink over tube, printed snoot, or machined. Decides G2's resolution and the sweep's duration | T8a |
| **O8** | **Pan arc and hard stops.** How far can it actually turn, and is there a mechanical stop or only the limit switches? Blocks sweep bounds (R13) and is entangled with §4.5's (i)/(ii) | T4 |
| **O9** | **Which G5 provider, and where the camera lives** — DVP into fabric or a third board publishing bearings over HCP (§4.7). A judgement call about how much of a vision project this becomes, not a technical toss-up | after T8b |
