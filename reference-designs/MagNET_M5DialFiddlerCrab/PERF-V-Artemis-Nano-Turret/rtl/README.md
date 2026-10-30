# rtl/ — turret motion controller

Verilog-2001 for the Perf-V (Artix-7 XC7A35T). **Simulated, not synthesized and
not on hardware.** This is PLAN.md **T5a** plus the parts of T3/T4 it stands on.

| File | What it is |
|---|---|
| `step_gen.v` | Modulo-`CLK_HZ` accumulator. `vel` is *exactly* steps/s — no scale factor, no divider |
| `phase_seq.v` | 8-state half-step table → ULN2003 IN1–IN4; all-zero when disabled |
| `motion_axis.v` | Trapezoidal planner, signed position counter, limit + e-stop gating |
| `sweep.v` | **T5a.** Walks an arc from one register write, strobing the host at each station |
| `perfv.xdc` | Vivado pin constraints, every pin read off the schematic. PerfXLab shipped none — this is the project's own, and the only record of the mapping |

```sh
brew install icarus-verilog   # once
sim/run.sh                    # ~20 s
```

This is the only part of the project that builds on macOS. Synthesis still needs
Vivado on the Windows bench.

## What the simulation proves

All three testbenches pass. `sim/tb_sweep.v` is T5a's acceptance criterion
verbatim — *"a full arc runs from one write; stations land on exact POS values;
abort is clean mid-sweep"*:

- 9 stations over `0 → 800` step `100`, each strobe latching the **exact**
  commanded position, with the axis provably stationary at every strobe.
- Reverse arcs. A step that doesn't divide the arc stops at the last station
  *inside* it (`0 → 250` step `100` gives 0, 100, 200 — it does not overshoot).
- A zero step is **refused**, not run forever.
- Abort during a move ramps the axis down, leaves it stopped, and re-arms.
- `vel` is drift-free: exactly `v` steps between two like-phased instants, at
  v = 1, 100, 997 and 50 000.
- e-stop halts within two clocks and nothing moves afterwards; a limit blocks
  motion *into* it while still allowing a back-off.

## Three things that came out of writing it

**1. The strobe goes *after* the dwell, and the dwell is two conversions.**
The OPT4048 integrates continuously, so a reading taken the instant the motor
stops is smeared across the arc just crossed. One conversion time is the
tempting number and it is wrong — it only guarantees that *a* conversion
finished, not that the one you read *began* after the axis was stationary. So
`dwell_ms ≥ 2 × conversion_ms`: **220 ms**, not 100 ms.

**2. Stop-and-shoot costs more in restarting than in dwelling.** Measured, for a
180° scan (2038 half-steps at the true 4075.77/rev, ±10° baffle → 113
half-steps/station, 220 ms dwell, vmax 2000 steps/s):

| | |
|---|---|
| Full scan, 19 stations | **8.35 s** |
| …of which dwell | 4.18 s |
| …of which travel | 4.17 s |
| Same arc travelled in **one** move | **2.48 s** |

Nineteen short triangular moves take 68% longer than one continuous move of the
same distance, because none of them is long enough to reach `vmax` — the axis
spends the whole scan accelerating and decelerating. So the on-the-fly sweep of
D13 is worth ~3.3×, not the modest win the plan implies. It is still not v1, but
that is now a known price rather than an assumption.

**3. The decel point is counted, not computed.** `n_acc` counts steps taken
while accelerating and deceleration begins when the steps remaining fall to it.
For a symmetric ramp that is exact, and it keeps the planner free of the
`v²/2a` that would otherwise want a divider. No multiply, no divide, no square
root anywhere in `motion_axis`.

## The configuration hazard, in one place

`PUDC_B` is pin **L15**, and on this board `R49` (pull-up to 3V3) is **not
fitted** while `R50` (1 kΩ to GND) **is** — so PUDC_B is held low and the
Artix-7 enables its internal pull-ups on **every** SelectIO pin for the whole of
configuration. The stepper phase pins are therefore not floating before
`FPGA_DONE`; they are actively pulled **high**, which is the one direction that
turns a ULN2003 on.

So **fit 1 kΩ pull-downs, not 10 kΩ.** Ten divides against a ~10 kΩ internal
pull-up to about 1.7 V, comfortably above the Darlington's turn-on; one holds
the node near 0.35 V. `DRIVE 12` on those pins in the `.xdc` is there because
the FPGA then has to sink 2.75 mA through the pull-down when driving high.
Nothing in the RTL or in Vivado fixes this — `PULLDOWN TRUE` and
`BITSTREAM.CONFIG.UNUSEDPIN` both apply only *after* configuration.

## Not verified by any of this

Simulation cannot see: synthesis and timing closure (expected trivial — well
under 5% of the 35T, one 26-bit adder in the critical path), real pin
assignment, the ULN2003 pull-downs of R3, whether `dir`=1 turns clockwise
(coil wiring — check once on the bench), and whether a 28BYJ-48 actually
follows these ramp parameters without stalling. `VMIN`, `vmax` and `accel`
are starting guesses, not measurements.
