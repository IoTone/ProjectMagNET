#!/usr/bin/env python3
# Regenerates the numbers in README 4.3. No inputs, no network; writes
# power-budget-data.json beside this file. See the README for provenance.
# Artemis Nano Multisense battery model. All currents in mA at the 3.3 V rail.
USABLE_MAH = 1600.0   # 2000 mAh nameplate x 80% to LDO dropout (see notes)

# --- component currents -------------------------------------------------
# BNO085, datasheet Fig 6-17 (VDDIO 3.0 V + VDD 3.3 V rails summed):
IMU_FUSION   = 3.50 + 7.50   # 9-axis Rotation Vector @100 Hz = 11.00 mA
IMU_SIGMO    = 0.40 + 0.14   # Significant Motion Detector @31.25 Hz = 0.54 mA
IMU_IDLE     = 0.12 + 0.01   # idle = 0.13 mA
IMU_BREAKOUT = 0.10          # Adafruit reg + level shifters (est.)

PERSON_ACTIVE  = 45.0        # vendor: ~150 mW / 3.3 V
PERSON_STBY_LO = 1.0         # UNDOCUMENTED - optimistic
PERSON_STBY_HI = 10.0        # UNDOCUMENTED - pessimistic

LED_RED   = 1.50             # D6 power LED, always on
LED_BLUE  = 1.00             # pad 19 status LED, full-on (est.)

MCU_NOPM  = 0.30             # 100 Hz SH-2 poll, no PM configured (est.)
MCU_PM    = 0.02             # suspend-to-ram, wake every 10 s (est.)
BLE_FAST  = 0.25             # advertising @125 ms (BT_LE_ADV_CONN_FAST_2) (est.)
BLE_SLOW  = 0.04             # advertising @1 s (est.)

PULLUP_NOPM = 0.50           # 4 parallel Qwiic pull-up sets, 100 Hz traffic (est.)
PULLUP_PM   = 0.01

LDO      = 0.055             # AP2112K-3.3 quiescent
GAUGE    = 0.015             # LC709203F
RTC      = 0.000022          # RV-1805
OPT_CONT = 0.024             # OPT4048 continuous
OPT_SHOT = 0.002             # one-shot 100 ms / 10 s
CH340    = 0.50              # UNMEASURED - README says "hundreds of uA to mA"

def other(opt, pullup, ch340):
    return LDO + GAUGE + RTC + opt + pullup + ch340 + IMU_BREAKOUT

def person_dc(standby, on_s, period_s):
    d = on_s / period_s
    return standby * (1 - d) + PERSON_ACTIVE * d

S = []  # label, {category: mA}, note
def add(label, group, **kw):
    S.append((label, group, kw))

# ---- Group 1: firmware as written today (fw 0.5.0-m4), bench config ----
base_today = dict(imu=IMU_FUSION, mcu=MCU_NOPM + BLE_FAST,
                  other=other(OPT_CONT, PULLUP_NOPM, CH340))

add("Today, as built", "today", **base_today, red=LED_RED, blue=LED_BLUE*0.50)
add("Today, red LED removed", "today", **base_today, red=0.0, blue=LED_BLUE*0.50)
add("Today, both LEDs removed", "today", **base_today, red=0.0, blue=0.0)
add("+ Person Sensor, continuous", "today", **base_today, red=LED_RED,
    blue=LED_BLUE*0.032, person=PERSON_ACTIVE)
add("+ Person Sensor, 250 ms / 10 s (standby 1 mA)", "today", **base_today,
    red=LED_RED, blue=LED_BLUE*0.50, person=person_dc(PERSON_STBY_LO, 0.25, 10))
add("+ Person Sensor, 250 ms / 10 s (standby 10 mA)", "today", **base_today,
    red=LED_RED, blue=LED_BLUE*0.50, person=person_dc(PERSON_STBY_HI, 0.25, 10))

# ---- Group 2: M5 low-power target ----
base_m5 = dict(mcu=MCU_PM + BLE_SLOW, other=other(OPT_SHOT, PULLUP_PM, 0.0))
add("M5 target, no Person Sensor", "m5", imu=IMU_SIGMO, **base_m5)
add("M5 + Person Sensor on a hard power switch", "m5", imu=IMU_SIGMO, **base_m5,
    person=PERSON_ACTIVE * (2.5 / 3600))        # 10 wakes/hour x 250 ms
add("M5 + Person Sensor parked in standby (1 mA)", "m5", imu=IMU_SIGMO, **base_m5,
    person=PERSON_STBY_LO + PERSON_ACTIVE * (2.5 / 3600))

CATS = ["person", "imu", "red", "blue", "mcu", "other"]
rows = []
for label, group, kw in S:
    parts = {c: round(kw.get(c, 0.0), 4) for c in CATS}
    tot = sum(parts.values())
    h = USABLE_MAH / tot
    rows.append((label, group, parts, tot, h))
    print(f"{label:48s} {tot:7.3f} mA  {h:8.1f} h  {h/24:7.2f} d")
    print("      " + "  ".join(f"{c}={parts[c]:.3f}" for c in CATS if parts[c]))

import json, pathlib
out = pathlib.Path(__file__).resolve().parent / "power-budget-data.json"
out.write_text(json.dumps(
    [{"label": l, "group": g, "parts": p, "total": round(t, 3),
      "hours": round(h, 1), "days": round(h / 24, 2)} for l, g, p, t, h in rows], indent=1))
print(f"\nwrote {out}")
