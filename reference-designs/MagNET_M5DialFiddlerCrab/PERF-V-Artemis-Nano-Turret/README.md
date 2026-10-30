# PERF-V-Artemis-Nano-Turret

A two-board hive member that aims a stepper at something it senses — light
first, a person second, and whatever a later module can see or hear after that.

**Status (2026-10-30): T5a done in simulation.** The motion controller and the
sweep primitive are written and pass three testbenches — see
[`rtl/`](rtl/README.md), run with `sim/run.sh`. Nothing is synthesized, no
hardware is wired and no firmware is built. Plan: [`docs/PLAN.md`](docs/PLAN.md).

`rtl/` is the only part of this project that builds on macOS.

| Board | Part | Job |
|---|---|---|
| **Perf-V** | Xilinx Artix-7 **XC7A35T-1FTG256C** (PerfXLab / Seeed `102110277`, $79, **discontinued**) | Motion controller: step generation, acceleration, position, limits, e-stop. No radio |
| **Artemis Nano** | Ambiq Apollo3 Blue, Zephyr 4.4.2 | Senses and the only radio. Bridges hive ↔ Perf-V |
| **Motor** | ELEGOO ULN2003 + 28BYJ-48, 5 V unipolar | ~0.088°/half-step, ~10–15 RPM, ~34 mN·m |

Sensors, reduced from `ArtemisNano_Hive_Multisense` to two:
**OPT4048** colour `0x44` · **Person Sensor** `0x62`. The BNO085 IMU, RV-1805 RTC and
LC709203F fuel gauge are dropped — there is no battery, so the Artemis is powered from
the Perf-V's 5 V and the Person Sensor just stays in continuous mode.

## Shape of it

```
Person Sensor ──I2C──► Artemis ──UART 115200──► Perf-V ──► ULN2003 ──► 28BYJ-48
                          │         (binary frame)   │
                          │                          └── limits, e-stop (in fabric)
                          └──BLE / HCP──► magnet_app, hive
```

The Artemis proxies HCP verbs prefixed `T.` to the Perf-V, so the turret is
reachable from the phone with no new protocol.

## Goals

| # | Goal | Status |
|---|---|---|
| **G1** | Aim where it is told — `T.MOVE <angle>`, ramped, with real limits | planned |
| **G2** | **Orient toward light by sweeping** the arc and turning to the brightest bearing | **primary v1 behaviour** |
| **G3** | Choose *which* light, by CIE xy / CCT — prefer a 2700 K lamp over a 6500 K window | planned, no extra RTL |
| **G4** | Track a face, closed loop on the Person Sensor inside its ~55° FOV | planned |
| **G5** | Take a camera or mic module later as a new bearing provider | designed for, not built |

The idea that makes G2–G5 one feature: a sensor either has a direction in a
single reading or it does not. **Lux and colour do not** — the bearing comes from
the motion, so you sweep and the turret becomes the sensor's aperture. **A face
box, a camera frame and a mic array do** — they give a bearing standing still,
inside a narrow field of view. So: **scan to acquire, frame to track**, and above
the UART everything is the same `{bearing_deg, confidence, age_ms, source}`. The
Perf-V never learns which sensor produced it. Plan §1.3, §4.6, §4.7.

## Two FPGA options

They are **stages, not alternatives**: Option B is Option A plus a CPU writing the
same registers. Step generation stays in RTL either way.

- **Option A — pure RTL.** UART + binary frame decoder + ramp + step generator.
  Vivado ML Standard only, no RISC-V SDK. Built first.
- **Option B — RISC-V soft core.** X-Core / Hummingbird E200 / SCR1 / WH32 (100T
  only) above the same peripheral. Gated on Option A working.

## What the FPGA actually buys

Real: jitter-free step timing (±20 ns), a hardware acceleration ramp the 28BYJ-48
genuinely needs, two synchronized axes for the cost of one instantiation,
limit/e-stop gating in a single clock, exact position by construction, and a free
front panel from the board's own switches and LEDs.

Not: any connectivity (it has none), loop bandwidth (the Person Sensor's ~7 Hz is
the cap), low power (wall-powered), or microstepping (a ULN2003 has no current
control). The accuracy limits — 1–2° of gearbox backlash and the 1:63.68395 gear
ratio — are mechanical, and no controller fixes them.

## Before touching anything

- **The OPT4048 needs a baffle, and G2 does not work without one.** Bare, the
  sensor sees ~±80°, so a 180° sweep changes the reading by a few percent and
  there is no peak to find. A tube over it — ~±10° is the sweet spot — is what
  sets the angular resolution, not the stepper's 0.088°. **Plot its response
  against a lamp at a known bearing before writing any peak-finding code.**
- **Fit 10 kΩ pull-downs on ULN2003 IN1–IN4.** FPGA pins float until `FPGA_DONE`,
  and floating Darlington inputs can energize the motor during configuration.
- **Do not use 4096 half-steps/rev.** The gear ratio is 1:63.68395, so a
  revolution is 4075.77 half-steps. The round number costs ~1.8° per revolution.
- **The motor gets its own 5 V supply**, common ground — not the Perf-V's `5V_IN`.
- Both toolchains (Vivado, Zephyr) are Windows-only. Nothing here builds on macOS.
- **The Perf-V is discontinued and there is no spare.** Archive a known-good bitstream in this
  repo from the first successful flash onward, and depend on Vivado and the schematic rather than
  on PerfXLab's servers, email or forum.
