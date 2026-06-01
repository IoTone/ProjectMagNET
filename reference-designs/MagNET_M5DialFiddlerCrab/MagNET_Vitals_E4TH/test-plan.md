# MagNET_Vitals_E4TH — Test Plan

**Date:** 2026-05-03
**Scope:** firmware on the Seeed MR60BHA2 mmWave kit (XIAO ESP32-C6 + radar + WS2812 + BH1750). Phases 1–3 landed; Phase 4 (mDNS + UC3 manifest) and Phase 5 (privacy banner) are out of scope.
**Validation model:** firmware is hardware-coupled — no unit-test runner on-device. Validation is **bench testing** with a documented procedure, recorded results, and a per-phase acceptance bar.
**Companion docs:** [`README.md`](README.md), [`specs/MagNET-Vitals-E4TH-proposal.md`](../../../specs/MagNET-Vitals-E4TH-proposal.md), per-component READMEs under `components/*/`.

---

## 1. Why a separate plan from `d3-spatial/test-plan.md`

The d3-spatial plan covers software tests on a developer's workstation (vitest, Playwright smoke, future visual diff). That model doesn't apply here:

- The firmware needs the kit, USB power, and a WiFi network.
- Most paths exercise hardware (radar UART frames, I²C lux reads, WS2812 timing, NimBLE GATT, lwip TCP).
- The "system under test" is a firmware build flashed onto a specific device, not a function returning a value.

So this plan is procedural, not automated. Each procedure has a numbered ID (e.g. `P3-HTTP-PHASES-04`), an acceptance criterion, and a place to record pass/fail in a log file.

A **future** extension could host-test the parser pieces (Tiny-Frame decoder, JSON formatters) on Linux against canned byte streams. That work is queued under §11.

---

## 2. Bench tooling

### Required
- Seeed MR60BHA2 mmWave kit (XIAO ESP32-C6 onboard).
- USB-C cable (data, not charge-only).
- Workstation with `esp-idf` ≥ 5.2 **or** PlatformIO (`espressif32@6.10.0`).
- A 2.4 GHz WiFi network whose SSID and password fit in 32/64 chars respectively.
- A BLE-capable phone with **nRF Connect** (Nordic) or **LightBlue** (Punch Through). Either works; nRF Connect is more featureful for arbitrary GATT writes.
- `curl`, `jq` for HTTP smoke (`brew install jq` / `apt install jq`).

### Recommended
- A second person — multi-target tests are easier with company than with a chair playing the second role.
- A chest-strap HR monitor (Polar H10 or similar) — for §8 cross-validation.
- A handheld lux meter — for §5.2 ground-truth.
- A Wireshark + tcpdump rig if you suspect packet-level issues.

### Optional (deeper diagnostics)
- Logic analyzer on the radar UART line (Saleae or PulseView + cheap FX2) — required only if checksum mismatches persist after the §4.4 procedure.
- BLE sniffer (Ubertooth, nRF52 dongle in sniff mode) — only for GATT-handshake debugging; provisioning-over-app rarely needs this.

---

## 3. Pre-flight (before first flash)

| ID | Procedure | Pass when |
|---|---|---|
| `PF-01` | Visual inspection: kit board has no shorts, capacitors not blown, USB-C connector clean | clean |
| `PF-02` | `lsusb` / Device Manager shows the C6 USB-CDC enumerating | a `Espressif`/`USB JTAG/serial` device appears within 2 s of plug-in |
| `PF-03` | `idf.py set-target esp32c6 && idf.py build` completes | exit 0; binary `<= 1.5 MB` |
| `PF-04` | `pio run -e esp32c6` completes | exit 0 |
| `PF-05` | `idf.py menuconfig` → "MagNET Vitals" submenus visible | three submenus: MR60BHA2, Status LED, BH1750 |
| `PF-06` | First flash + `idf.py monitor` shows the boot banner | `MagNET Vitals E4TH — Phase 3` line appears within 1 s of reset |

If any pre-flight fails, **stop** and resolve before continuing.

---

## 4. Phase 1 — radar UART driver (`craw_mr60bha2`)

The radar starts emitting Tiny-Frame data ~2 s after power-on. Acceptance: parser sees valid frames, dispatches them, populates state, and the LED transitions out of boot.

### 4.1 Frame arrival

| ID | Procedure | Pass when |
|---|---|---|
| `P1-FRAMES-01` | Power-on, monitor, no presence in cone for 30 s | LED reaches IDLE (slow cyan pulse) within ~3 s; no `cam_hal:`/`UART error`/`buffer overflow` lines in log |
| `P1-FRAMES-02` | Enable debug logging (`CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y`), reflash, watch | `[craw_mr60] init ok port=…` once; afterwards no `header cksum mismatch` or `data cksum mismatch` for 30 s |
| `P1-FRAMES-03` | At REPL: `mr60-status` while no presence | prints `radar fw=0x…  last_frame=N ms ago` with `N < 500` |
| `P1-FRAMES-04` | Sit in cone, repeat `mr60-status` | `N` stays bounded (frames keep arriving) |

### 4.2 Type coverage

Stand in cone, pause 5 s, then at REPL:

| ID | Procedure | Pass when |
|---|---|---|
| `P1-TYPES-01` | `vitals` | Every field non-zero except possibly `Lux` (depends on BH1750); `Phase: t b h` triplet has plausible (non-NaN) floats |
| `P1-TYPES-02` | `presence?` | pushes `1` |
| `P1-TYPES-03` | `hr?` | pushes a value in [40, 150] |
| `P1-TYPES-04` | `br?` | pushes a value in [4, 30] |
| `P1-TYPES-05` | `targets?` | pushes ≥ 1 (one target = you) |
| `P1-TYPES-06` | `distance?` | pushes `> 30` (cm) and `< 200` for typical seated posture |

### 4.3 Negative path — no person

Step out of the cone for ≥ 10 s, then:

| ID | Procedure | Pass when |
|---|---|---|
| `P1-EMPTY-01` | `presence?` | pushes `0` |
| `P1-EMPTY-02` | `hr?` / `br?` | push values that decay or stay near 0 (radar emits HR/BR frames intermittently when no presence; 0 or last-known is OK) |
| `P1-EMPTY-03` | `targets?` | pushes `0` |

### 4.4 Checksum integrity (only if §4.1 fails)

If `P1-FRAMES-02` shows persistent checksum mismatches:

| ID | Procedure | Pass when |
|---|---|---|
| `P1-CKSUM-01` | Verify `idf.py menuconfig` UART pins match the kit schematic | RX/TX swap fixes most "header cksum mismatch" sprays |
| `P1-CKSUM-02` | Verify baud is 115 200 (default) | `Kconfig` doesn't expose this; defined in `craw_mr60bha2.c`. |
| `P1-CKSUM-03` | Logic analyzer: capture 1 s of UART traffic, look for `01 ` start byte and 8-byte header pattern | first byte after a quiet gap is `0x01`; lengths in bytes 3–4 are reasonable (< 100) |
| `P1-CKSUM-04` | If still failing: dump `xxd` of raw UART, check for big-endian/little-endian assumption | header is BE for LEN/TYPE; payload is LE for floats — confirm with capture |

---

## 5. Phase 1.5 — status LED + lux sensor

### 5.1 LED modes

Power-on with no presence, follow this sequence:

| ID | t (s) | Expected LED | Pass when |
|---|---|---|---|
| `P15-LED-01` | 0 | solid amber | visible immediately |
| `P15-LED-02` | 0–3 | solid amber | stable, no flicker |
| `P15-LED-03` | ~3 | transition to slow cyan pulse | smooth fade, no glitch |
| `P15-LED-04` | 3–∞ (no presence) | slow cyan pulse, ~4 s period | sinusoidal, never fully off |
| `P15-LED-05` | enter cone | switch to BPM-hued pulse, ~2 s period | hue shifts noticeably; faster pulse than IDLE |
| `P15-LED-06` | hold for 60 s | hue tracks BPM changes | watch — calmer person = bluer; hold breath briefly to bump HR up = redder |
| `P15-LED-07` | leave cone, wait 10 s | back to cyan IDLE | smooth transition |
| `P15-LED-08` | at REPL: `0 led-vitals` | LED off | dark within one tick (~50 ms) |
| `P15-LED-09` | at REPL: `1 led-vitals` | LED back to current mode | resumes immediately |

If LED is dark: confirm `CONFIG_CRAW_LED_GPIO` matches your board (default 21).

### 5.2 BH1750 lux

| ID | Procedure | Pass when |
|---|---|---|
| `P15-LUX-01` | At REPL: `lux?` in normal room light | pushes value in [50, 1500] (typical office/home) |
| `P15-LUX-02` | Cover sensor with hand | next `lux?` pushes value `< 20` |
| `P15-LUX-03` | Shine phone flashlight directly at sensor | pushes value `> 5000` (could saturate at 65 535 — that's a pass) |
| `P15-LUX-04` | Compare against a handheld lux meter at the same orientation | within ±20 % across 3 light levels (low/medium/bright) |
| `P15-LUX-05` | Disconnect sensor (yank a Grove cable) and reset | boot log shows `bh1750 init failed` warning; firmware continues; `lux?` pushes `-1`; `/lux` returns `{lux: null, error: "i2c_read_failed"}` |

---

## 6. Phase 2 — provisioning + Forth

### 6.1 Boot sequence

| ID | Procedure | Pass when |
|---|---|---|
| `P2-BOOT-01` | First-ever flash, `idf.py monitor` | banner → `BLE advertising as 'MagNET-vitals-<MAC4>'` → `Forth engine initialized.` → `Free heap: <N>` → REPL prompt |
| `P2-BOOT-02` | Free heap reported in banner | `> 150 KB` (after Forth's 64 KB allocation) |
| `P2-BOOT-03` | REPL responsive: type `1 2 + .` | prints `3` |
| `P2-BOOT-04` | REPL: `words` | prints all built-in words plus the 13 vitals/provisioning words |

### 6.2 BLE provisioning (no creds yet)

| ID | Procedure | Pass when |
|---|---|---|
| `P2-BLE-01` | Open nRF Connect, scan | `MagNET-vitals-<MAC4>` appears within 5 s |
| `P2-BLE-02` | Connect to it | services `0x1800`, `0x1801`, and `4d41474e-4554-0001-…` enumerate |
| `P2-BLE-03` | Read characteristic `01` (device_info) | returns JSON with `chip`, `role: "vitals"`, `fw`, `ssid`, `ip`, `status` |
| `P2-BLE-04` | Write SSID to char `02` (UTF-8, ≤ 32 bytes) | console prints `[PROV] creds received: ssid='…' pass=N chars` |
| `P2-BLE-05` | Write password to char `03` | ack only; reads return empty |
| `P2-BLE-06` | Write `0x01` to char `04` (commit) | console prints `[PROV] commit -> connecting to '…'`; LED stays in current mode |
| `P2-BLE-07` | Within 30 s | console prints `[WiFi] connected, IP: x.x.x.x`; char `05` notifies new IP; char `06` (status) notifies `4` (CONNECTED) |
| `P2-BLE-08` | At REPL: `prov-status` | shows `prov: CONNECTED`, `wifi: connected`, `ssid: <yours>`, `ip: x.x.x.x` |

### 6.3 Auto-connect on reboot

| ID | Procedure | Pass when |
|---|---|---|
| `P2-RECONNECT-01` | After §6.2, press the C6's RESET button | banner → `Stored WiFi '…' — auto-connecting...` → `[WiFi] connected` within 10 s |
| `P2-RECONNECT-02` | Without rebooting, drop the WiFi network (router off / disable SSID) | `[WiFi] disconnected` log; `prov-status` shows `wifi: down` |
| `P2-RECONNECT-03` | Bring the network back up | reconnects automatically within ~30 s |
| `P2-RECONNECT-04` | At REPL: `prov-reset` | clears creds; `[WiFi] disconnected`; resumes BLE advertising; LED unchanged |
| `P2-RECONNECT-05` | Reboot after `prov-reset` | banner shows `No stored WiFi. Connect via BLE to provision.` |

### 6.4 Forth word coverage

For each word, give a stack-trace expectation. The full vocabulary is 13 words:

| Word | Test | Pass when |
|---|---|---|
| `hr?` | `hr? .` while in cone | prints integer 40–150 |
| `br?` | `br? .` while in cone | prints integer 4–30 |
| `presence?` | `presence? .` outside / inside cone | prints `0` / `1` |
| `distance?` | `distance? .` while in cone | prints integer 30–200 (cm) |
| `lux?` | `lux? .` (cover/uncover sensor) | meaningful value changes |
| `targets?` | `targets? .` solo / with someone else | `1` / `2` |
| `vitals` | `vitals` | prints labelled snapshot, all fields formatted |
| `hr-history.` | `hr-history.` after ≥ 2 min runtime | prints ≥ 1 sample row; rows are chronological |
| `br-history.` | same | same |
| `mr60-status` | `mr60-status` | `last_frame=<small ms>` |
| `led-vitals` | `0 led-vitals` then `1 led-vitals` | LED off / on |
| `prov-status` | already covered in §6.2 | — |
| `prov-reset` | already covered in §6.3 | — |

Composability check (proves FFI works as a Forth primitive, not just a blackbox):

```forth
ok> presence? if hr? . ." bpm  " br? . ." rpm" else ." nobody home" then
72 bpm  14 rpm
ok>
```

`P2-FORTH-COMPOSE-01`: composition above produces sensible output. **Pass:** yes.

---

## 7. Phase 3 — HTTP services

Find the device IP from `prov-status` and export it: `DEV=192.168.1.42`.

### 7.1 Per-endpoint smoke

| ID | Procedure | Pass when |
|---|---|---|
| `P3-HTTP-VITALS-01` | `curl -s http://$DEV/vitals \| jq .` | parses as JSON; has all of `bpm,rpm,presence,distance_cm,range_flag,lux,total_phase,breath_phase,heart_phase,target_count,fw_version,timestamp_us` |
| `P3-HTTP-HR-01` | `curl -s http://$DEV/heart-rate` | `{bpm, presence, timestamp_us}` valid JSON |
| `P3-HTTP-HRH-01` | `curl -s http://$DEV/heart-rate/history \| jq '.samples \| length'` | ≥ 1 after a few minutes; ≤ 60 |
| `P3-HTTP-HRH-02` | history `t` values | strictly increasing (chronological) |
| `P3-HTTP-BR-01` | `curl -s http://$DEV/breathing` | parses; `rpm` numeric |
| `P3-HTTP-BRH-01` | history shape | same as HR history |
| `P3-HTTP-PRES-01` | `curl -s http://$DEV/presence` | `{present, distance_cm, age_ms, timestamp_us}` |
| `P3-HTTP-PRES-02` | step out of cone, repeat | `present:false`, `distance_cm:0` |
| `P3-HTTP-LUX-01` | `curl -s http://$DEV/lux` | `{lux: <num>, timestamp_us}` |
| `P3-HTTP-LUX-02` | with sensor disconnected | `{lux: null, error: "i2c_read_failed"}` (regression for §5.2 P15-LUX-05) |
| `P3-HTTP-TGT-01` | `curl -s http://$DEV/targets \| jq .` | `count` matches `targets` array length; per-entry `id, x_m, y_m, dop, cluster` present |
| `P3-HTTP-TGT-02` | with no presence | `{count: 0, targets: []}` |
| `P3-HTTP-PHASES-01` | `curl -s http://$DEV/phases \| jq 'length'` | exactly `3` |
| `P3-HTTP-PHASES-02` | `curl -s http://$DEV/phases \| jq '[.[] \| length] \| unique'` | one element only — all three channels same length, ≤ 200 |
| `P3-HTTP-PHASES-03` | repeat 5× over 10 s | length grows up to 200 then stays stable |

### 7.2 CORS preflight

| ID | Procedure | Pass when |
|---|---|---|
| `P3-CORS-01` | `curl -i -X OPTIONS http://$DEV/vitals` | `204 No Content`; headers include `Access-Control-Allow-Origin: *`, `Access-Control-Allow-Methods: GET, OPTIONS`, `Access-Control-Max-Age: 86400` |
| `P3-CORS-02` | repeat for `/heart-rate`, `/phases`, `/targets` | wildcard `OPTIONS /*` matches them all |
| `P3-CORS-03` | `curl -i http://$DEV/vitals \| grep -i access-control-allow-origin` | header present on the GET response too |

### 7.3 Behaviour under WiFi flap

| ID | Procedure | Pass when |
|---|---|---|
| `P3-FLAP-01` | While `curl --keepalive http://$DEV/vitals` running, drop the WiFi | server stops within seconds; new connections refused |
| `P3-FLAP-02` | Bring WiFi back, retry | server auto-restarts (visible in console: `[HTTP] vitals server up at http://…`) |
| `P3-FLAP-03` | `prov-reset` then re-provision | new HTTP server starts at the new IP |

### 7.4 JSON robustness

| ID | Procedure | Pass when |
|---|---|---|
| `P3-JSON-01` | `for ep in vitals heart-rate breathing presence lux targets phases heart-rate/history breathing/history; do curl -s http://$DEV/$ep \| jq -e . >/dev/null && echo OK $ep; done` | every endpoint prints `OK`. Any `jq` parse error is a malformed-JSON regression. |
| `P3-JSON-02` | `curl -s http://$DEV/vitals \| jq -e '.lux \| type'` | `"number"` when sensor present, `"null"` when disconnected. **No** `string` or `object`. |

---

## 8. Cross-cutting

### 8.1 HR / BR cross-validation (optional)

Use a chest-strap HR monitor connected to a phone (Polar Beat / Garmin Connect) as ground truth.

| ID | Procedure | Pass when |
|---|---|---|
| `XV-HR-01` | Sit calmly for 60 s; record radar HR via `hr?` once per 10 s and chest-strap HR at the same instants | five paired readings; mean abs error < 8 BPM at rest |
| `XV-HR-02` | Hold breath for ~20 s, then exhale; both should bump | both rise; radar lag < 10 s vs strap |
| `XV-BR-01` | Breathe at 6/min metronomic, 30 s | radar `br?` settles in [4, 8] |
| `XV-BR-02` | Breathe at 20/min, 30 s | radar `br?` settles in [16, 24] |

### 8.2 Multi-target behaviour

| ID | Procedure | Pass when |
|---|---|---|
| `MT-01` | One person in cone | `/targets` returns `count: 1` |
| `MT-02` | Second person enters cone | `count: 2`; both have plausible `x_m, y_m`; `id` values are distinct |
| `MT-03` | Second person leaves; wait 30 s | `count: 1`; remaining target's `id` may persist or change (radar-side behaviour) |
| `MT-04` | Three people fan out at ~1 m, 1.5 m, 2 m | `count` reaches `3` (the radar's max) |
| `MT-05` | All leave, wait 30 s | `count: 0`; LED returns to IDLE |

### 8.3 Posture envelope (Seeed sleep-only caveat)

The kit is spec'd for sleep posture (1 m height, 45° tilt). Verify the documented degradation outside that envelope:

| ID | Procedure | Pass when |
|---|---|---|
| `POS-01` | Mount as spec'd; recline below it | HR/BR stable, signal_q implied by minimal jitter |
| `POS-02` | Mount on a desk, sit in front of it (ergonomic anti-pattern) | readings still arrive but jitter visibly higher; the demo framing of "non-clinical" is honest |
| `POS-03` | Hold a book between the radar and your chest | HR collapses or jitters wildly — **expected**; firmware doesn't lie about signal |

There is no firmware-side acceptance for "accuracy"; we're documenting that the system's rendering of low-confidence data is consistent with reality (proposal §1.1).

---

## 9. Negative tests

| ID | Procedure | Pass when |
|---|---|---|
| `NEG-01` | Power-on with radar UART disconnected | parser reports no frames; `mr60-status` shows `last_frame=-1 ms ago` (or large positive); LED stays IDLE; HTTP `/heart-rate` returns `bpm:0,presence:false`; firmware doesn't crash |
| `NEG-02` | Power-on with BH1750 disconnected | `bh1750 init failed` log; `lux?` pushes `-1`; `/lux` returns null+error; firmware continues |
| `NEG-03` | Provision with wrong WiFi password | `[WiFi] connect failed`; status `5` (FAILED) notified on char `06`; `prov-status` shows `FAILED` |
| `NEG-04` | Mid-history fetch, hit `prov-reset` | server stops, response truncated cleanly (no partial JSON parse on next connect — connection refused, then re-provision starts a fresh server) |
| `NEG-05` | `/phases` after a fresh boot, before any HB-phase frame arrived | returns `[[],[],[]]` (empty arrays) — valid JSON, no error |
| `NEG-06` | Out-of-range Forth: `999 led-vitals` | LED on; tolerated as truthy. (Document, not fix — Forth users live with their stack.) |
| `NEG-07` | `prov-reset` while WiFi is up but radar is mid-frame | no UART deadlock; HTTP server stops; radar parser keeps running |

---

## 10. Soak / stress

These are 8 h to 24 h runs that catch slow leaks and wedged tasks.

| ID | Procedure | Pass when |
|---|---|---|
| `SOAK-01` | 24 h power-on, no provisioning | every 1 h: monitor still responsive; `Free heap` from `mr60-status`+ a Forth heap-print word doesn't drop monotonically; LED still pulsing |
| `SOAK-02` | 8 h with WiFi up, fire `curl /vitals` once a second from a workstation | response time stable; no growing HTTP error rate; `idf.py monitor` shows no `httpd_*` warnings |
| `SOAK-03` | Same as SOAK-02 plus `curl /phases` once a second | `/phases` continues to return ≤ 200 samples; ring buffer wraps cleanly |
| `SOAK-04` | Force a WiFi flap once an hour for 12 h | server restarts each time; no double-start crash |
| `SOAK-05` | Repeated provisioning cycles (provision → reset → provision) ×100 | NVS doesn't fragment to failure; `craw_nvs_save_wifi_creds` succeeds on attempt 100 |

For SOAK-01: add a temporary Forth word that prints heap free; sample once an hour. Strictly monotonic decay > 5 KB/h is a leak worth chasing.

---

## 11. Automation

### Available now

- **`tests/http-smoke.sh <ip>`** — automates every `P3-HTTP-*` and `P3-CORS-*`
  procedure from §7. ~260 lines of bash, deps `curl` + `jq`, exits 0 on
  full pass / 1 on any test failure / 2 on usage error. Run after WiFi
  is up:
  ```bash
  ./tests/http-smoke.sh 192.168.1.42        # IP from `prov-status`
  ./tests/http-smoke.sh 192.168.1.42 -v     # show response bodies on fail
  DEV=192.168.1.42 ./tests/http-smoke.sh    # env-var form
  ```
  Output uses the same procedure IDs as this plan, so a failed run
  copy-pastes directly into the §13 test log.

### Queued for future cycles

- **Host-test the Tiny-Frame parser** in plain C against a captured byte stream (record 60 s of UART traffic with a logic analyzer, replay through `craw_mr60bha2.c`'s parser linked into a Linux test binary).
- **Host-test the JSON formatters** in `http_vitals.c` — same idea, but feed a synthetic `craw_mr60_state_t` and assert the output round-trips through `jq`.
- **BLE provisioning smoke** — `bluetoothctl` / `gatttool` automation of §6.2 once a stable Linux runner is in scope. Manual via nRF Connect for now.

---

## 12. Acceptance bar per phase

| Phase | Acceptance |
|---|---|
| Phase 1 | All `P1-FRAMES-*` and `P1-TYPES-*` pass; no checksum mismatches in 30 s |
| Phase 1.5 | All `P15-LED-*` and `P15-LUX-*` pass; lux ground-truth within ±20 % |
| Phase 2 | All `P2-BOOT-*`, `P2-BLE-*`, `P2-RECONNECT-*` pass; every Forth word in §6.4 produces the expected stack/print effect |
| Phase 3 | All `P3-HTTP-*` and `P3-CORS-*` pass; `P3-JSON-01` shows OK for every endpoint |
| Cross-cutting | `MT-01..05` pass with two helpers; `XV-HR-*` mean error reported (informational) |
| Soak | At least `SOAK-01` and `SOAK-02` over 8 h |

A device passes acceptance when **every** procedure in its phase column above produces the documented "Pass when" outcome.

---

## 13. Test log format

Record runs under `MagNET_Vitals_E4TH/test-logs/<YYYY-MM-DD>-<short-name>.md`. Suggested template:

```markdown
# Test Run — 2026-05-04 — first-bench-up

**Tester:** dkords
**Build:** git rev-parse HEAD: <hash>
**Hardware:** Seeed MR60BHA2 kit, S/N <serial>
**WiFi:** HomeWiFi (2.4GHz, WPA2)
**Phone:** iPhone 15, nRF Connect 2.7.x

## Procedure results

| ID | Result | Notes |
|---|---|---|
| PF-01 | pass | |
| PF-06 | pass | banner @ 870 ms |
| P1-FRAMES-01 | pass | LED reaches IDLE @ 3.2s |
| P1-FRAMES-03 | pass | last_frame=63 ms |
| ...
| P3-HTTP-PHASES-02 | fail | jq returned `[120,120,180]` once during the 5th retry — investigating ring-buffer race |

## Open issues

1. (P3-HTTP-PHASES-02) suspect race between handler reading from ring while parser pushes — mutex covers it but worth verifying. Repro: …
```

The log lives in the repo so future reviewers can see what was actually exercised, not just what was claimed to work.
