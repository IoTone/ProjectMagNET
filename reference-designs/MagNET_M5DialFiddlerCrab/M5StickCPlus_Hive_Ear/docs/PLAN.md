# M5StickCPlus_Hive_Ear — Plan

Status (2026-10-06): **E0, E1, E2 DONE on HW.** The Ear streams 16 kHz ADPCM
over ATVV v1.0 to a Mac host (`tools/ear_rx.py`): host MIC_OPEN and
button hold-to-talk both work, with 0 lost frames. E3 (Genio/NixOS) is
documented in README but not run on a Genio. E4 needs a POSIX hive client.

## 1. Goal

A new hive role, **Role 13: Ear** — "anything that can hear audible sounds,
capture them, and pass them along to interested parties. Special skill:
sharing audio streams." Should display a cute ear.

First implementation: **M5StickC Plus** (classic ESP32-PICO-D4, BT 4.2, no
PSRAM, 4 MB flash) used as a **C-3PO-style thumb mic**: hold button A, talk,
the consumer hears it. Consumer: **MediaTek Genio 720 running NixOS** (BT 5.3,
we control the whole stack down to the OS port). Voice quality is enough.

Push this board as far as it goes; the XIAO ESP32-S3 Sense is the known
fallback (PSRAM, BLE 5) but is out of scope for now.

## 2. Decisions (settled)

| # | Decision | Why |
|---|---|---|
| D1 | **Audio over BLE using the Android TV Voice-over-BLE profile ("ATVV")** — GATT, IMA-ADPCM. | Requirement: stay on an existing BLE spec, invent nothing. LE Audio (SIG BAP/LC3) needs BT 5.2 isochronous channels — impossible on the ESP32 (BT 4.2). HFP is Classic BT, needs Bluedroid + BR/EDR on a no-PSRAM chip next to WiFi; rejected. ATVV is the shipping voice-remote spec, built for exactly push-to-talk on BLE 4.x. |
| D2 | **Streams only when the consumer activates it**, per the profile: host writes `MIC_OPEN`; thumb button sends the profile's `AUDIO_START` request (push-to-talk). No hive-side start/stop. | "Behave like a real BLE device." |
| D3 | **The hive is the data-sync plane, never the audio path.** It carries discovery, status, and pairing material for the BLE stream. | Same split as Eye/Camera ("hive carries the sighting, HTTP carries the pixels"). KV values cap at 3 KB anyway. |
| D4 | **Bonded links only** (LE Secure Connections). Passkey shown on the StickC display. Audio characteristics require an encrypted link. | Bonding approved. A mic must not be openable by a passer-by. |
| D5 | **Start from the current hive node template, not from Voice_XR.** Structural base: `MagNET_ReSpeaker_Boombox` (leanest current node, full H-series role lifecycle, KV `<role>:cmd` + `<role>:status`). Classic-ESP32 specifics (UART console, BLE-only controller, deferred peripheral init) from `M5_Hive_Camera` `[env:m5camerax]`. Board knowledge harvested from `M5StickCPlus-Blinky_Voice_XR` (§6) — that project stays untouched. | User direction. |
| D6 | Role string `ear`, caps `{"mic","audio-in","atvv"}` — used in the record the consumer publishes. The ruler needs **no change** (roles are free-form ≤16 chars). | `craw_hive_ruler.c:372-411` echoes any requested role. |
| D7 | **The StickC Plus Ear is a BLE-only hive *accessory* (2026-10-06).** No WiFi, no provisioning, no hive client, no SNTP, no mDNS on the device. Its bonded consumer (Genio 720) reads the Ear over standard services (DIS, BAS, ATVV caps) and publishes `ear:<id>` into the hive on its behalf. | Measured in E0: WiFi init alone cost ~42 KB and left 33 KB free after NimBLE *before* connecting; BLE-only leaves 91.5 KB with a live link. Also: the single 2.4 GHz radio goes 100 % to BLE audio (no coex jitter), and the 120 mAh cell isn't drained by a WiFi STA. Revisit on a PSRAM board (StickC Plus2 / XIAO S3 Sense) if a self-joining Ear is wanted. Supersedes the shared-`craw_ble_provision` changes and the Boombox-template hive skeleton of D5. |

## 3. Architecture

```
 SPM1423 PDM mic (CLK G0, DAT G34)
   └─ i2s_pdm RX, continuous DMA ──► ring buffer (internal RAM)
        └─ ear_enc task: DC-block → gain → IMA-ADPCM (ATVV frame layout)
             └─ NimBLE GATT: ATVV service ──notify──► Genio 720 (BlueZ)
                                                       └─ ear-rx daemon → ADPCM decode
                                                          → snd-aloop → "Ear" ALSA capture
                     Genio ─(WiFi/LAN)─► hive: publishes ear:<id> for the Ear
```

The Ear itself has no WiFi (D7). Everything on air is a standard SIG service
or the ATVV profile.

### 3.1 BLE peripheral (`src/ear_ble.c`) — as built in E0

- NimBLE peripheral-only (central/observer roles compiled out), BLE-only
  controller, `MAX_CONNECTIONS=2`, preferred MTU 247.
- Classic-BT controller memory released before init
  (`esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)`: **+15 KB**).
- GAP name `MagNET-Ear-<bt mac4>`, appearance 0x0180 Generic Remote Control,
  advertising 100-150 ms, BAS UUID in the advert.
- **DIS 0x180A**: manufacturer `IoTone MagNET`, model, serial = hive node id
  (`MagNET-biologic-<bt mac4>`), firmware revision.
- **BAS 0x180F**: level from the AXP192, notify on change (30 s poll).
- Security: LE SC + bonding + MITM, IO cap DisplayOnly → passkey on screen;
  bonds in NVS (`CONFIG_BT_NIMBLE_NVS_PERSIST`); repeat-pairing handled;
  `ble-unpair` / hold B 5 s clears bonds. DIS/BAS are open-read; ATVV
  characteristics (E2) will require an encrypted link.

Bringup order: console → M5GFX/AXP192 (owns I2C port 1) → NVS → `forth_init`
(before BLE, or the dictionary heap starves) → BT mem release → NimBLE →
UI task → REPL. Mic/I2S lands last in E1 (GPIO0 strap).

### 3.2 ATVV profile (to confirm against the spec in E0)

From the published implementations (Nordic nRF Smart Remote `ble_atvv`, RDK
voice): service `AB5E0001-5A21-4F05-BC7D-AF01F617B664`, characteristics
`…0002` TX (host→remote write: GET_CAPS, MIC_OPEN, MIC_CLOSE),
`…0003` RX (audio notify), `…0004` CTL (control notify: AUDIO_START,
AUDIO_STOP, START_SEARCH, sync). v0.4 = 8 kHz IMA-ADPCM; v1.0 adds 16 kHz,
interaction models (on-request / press-to-talk / hold-to-talk) and audio
sync frames. **E0 task:** get the spec text, pin the version, frame sizes and
MTU requirements. Target: v1.0, 16 kHz, hold-to-talk model. Fallback 8 kHz
if DRAM or airtime says so.

### 3.3 Hive contract (data sync only — published by the consumer)

The consumer, not the Ear, writes the hive record, built purely from what
it reads over the bonded link:
- `ear:<mac4>` KV, on connect/disconnect/state change + periodic:
  `{"t","node":"<DIS serial>","bd_addr","model","fw","role":"ear",
  "caps":["mic","audio-in","atvv"],"profile":"atvv/1.0","codec":"adpcm/16000",
  "state":"away|idle|open|live","batt","host":"<consumer node id>"}`
- Optional Dial "ear live" dot keyed on `ear:*` state (pattern of the
  bridge/redis dots, `M5StackDial-m5gfx-demo-ESPIDFORTH/src/main.cpp:731-746`).
- Later, if wanted: signed role bundles to the Ear over BLE (Hanasu already
  delivers `craw_role_bundle` envelopes over BLE companion; the engine has no
  `craw_hive` dependency since H6).

**Dependency:** the consumer needs a hive client. `craw_hive` is ESP32-only
today; `docs/device-profile-MilkV-Duo256M.md` §5 sketches the POSIX port
(`craw_hive_port.h` shim + `craw_hive_node_posix.c`, reusing
`craw_hive_proto.c`). The Genio would be its first user.

## 4. Phases

Each phase ends on a hardware gate; nothing is "done" on a clean build alone.

### E0 — Scaffold + memory budget ✅ DONE (HW) 2026-10-06
- Project: `platformio.ini` (`magnet_base`, `board = m5stick-c`, dio, 4 MB),
  nvs 64 KB + factory 2.5 MB, UART console, CPU 240 MHz, `forth` symlink only.
- `src/board_stickc.cpp` — M5GFX display (no full-screen sprite), AXP192
  battery (mV-correct %), debounced A/B, LED. `src/ear_ble.c` — §3.1.
- First cut followed the Boombox hive template (WiFi + provisioning + hive
  join); the measurements below drove D7 and it was cut back to BLE-only.

Internal heap (free / largest block), StickC Plus, same build otherwise:

| Milestone | WiFi + hive cut | BLE-only (final) |
|---|---|---|
| boot | 144.4 / 110.6 KB | 164.8 / 110.6 KB |
| after Forth (32 KB) | 105.2 / 77.8 | 125.5 / 77.8 |
| after WiFi driver init | 63.0 / 61.4 | — |
| after Classic-BT release | 77.9 / 61.4 (+15 KB) | 140.4 / 77.8 |
| after NimBLE + advertising | **33.5 / 32.8** (WiFi not yet connected) | **95.9 / 77.8** |
| 1 central connected | — | **91.5 / 77.8** |

Flash: 1.12 MB → 650 KB. Mac (bleak) central: found by name prefix + BAS
UUID, read all DIS fields + BAS, MTU 247 negotiated, clean disconnect.
Not yet exercised: pairing/passkey (nothing requires encryption until ATVV).
Bench note: opening the CH552 port resets the Stick even with DTR/RTS low.

**Carried to E2:** get the ATVV spec text and write `docs/Ear-ATVV-profile.md`.

### E1 — Continuous capture + encoder ✅ DONE (HW) 2026-10-06
Result: `components/craw_mic` promoted (0.2.0, streaming API, RIGHT slot;
Voice_XR rebuilt green against it). `src/ima_adpcm.c` passes
`ear-selftest` at 32.3 dB. `src/ear_audio.c` does DC removal, a 120 Hz
biquad high-pass and an AGC. `tools/ear_dump.py` decodes `mic-dump` to WAV.
Four HW bugs found and fixed (README "gotchas" 1-4, incl. the 80 ms PDM
start-up transient): mic power on AXP192
GPIO0-LDO, RIGHT PDM slot, and sub-60 Hz drift (94-99 % of energy before
the HPF, 0.1 % after). A 1 kHz tone from the Mac speaker was detected at
exactly the played length.

Original scope:
- Promote `craw_mic` from Voice_XR to `components/craw_mic` (H10 policy: second
  consumer), add `craw_mic_stream_start(cb, ctx)` keeping the one-shot API so
  Voice_XR still builds against it.
- `components/craw_adpcm` — IMA-ADPCM encoder (+ decoder for self-test).
- DC blocker (1-pole HPF), fixed gain, peak/RMS meter.
- Forth: `mic-dump` (raw PCM over UART at 921600 → host WAV script),
  `ear-selftest` (encode→decode SNR).
- **Gate:** clean speech in the WAV; measure DC offset + noise floor and record
  them (Voice_XR never did); SNR of codec round-trip logged.

### E2 — ATVV on NimBLE ✅ DONE (HW) 2026-10-06
Result: `src/ear_atvv.c` implements ATVV v1.0. It sends CAPS_RESP (16 kHz
ADPCM, HTT, 128 B frames), handles MIC_OPEN/CLOSE/EXTEND with a 15 s
timeout, does hold-to-talk on button A, and emits AUDIO_SYNC at start,
every 25 frames and after any drop. A control task owns state; the NimBLE
callbacks only enqueue. The wire contract is in `docs/Ear-ATVV-profile.md`.
`tools/ear_rx.py` is the host. Measured with a Mac host:
- MIC_OPEN 6 s: 376 frames, 0 lost / 0 dropped; tone + speech intact,
  0 clipped samples.
- HTT ×2: 245 + 183 frames, 0 lost, `AUDIO_STOP 0x02`.
- Heap: 76.8 KB, flat.

Soak, 10 min MIC_OPEN with MIC_EXTEND every 5 s: 37,406 frames, 94
dropped (0.25 %, gate < 1 %). Every drop was concealed and re-synced; the
WAV is exactly 600.00 s. Heap 76,788 B free at 1, 5 and 10 min, min_ever
unchanged. The Ear is left on the default (passkey) build with its bonds
cleared.

**Passkey (MITM) pairing: validated by the user on macOS 2026-10-07 (see E2.1).**
Earlier note: The first attempt
timed out with no one at the macOS dialog, and the Ear correctly refused
(Insufficient Encryption). Streaming was validated with the bench-only
`-DEAR_DEV_JUSTWORKS=1` build; the default build requires the passkey.

Original scope:
- `components/craw_atvv` — GATT service, opcode state machine, frame pacer,
  drop-and-count on `os_mbuf` exhaustion (never block capture).
- ATVV characteristics require an encrypted, bonded link — first real
  exercise of the E0 pairing path (passkey on display, bonds in NVS).
- Button A = hold-to-talk → `AUDIO_START`/`AUDIO_STOP`; display "LIVE" + red
  LED while open.
- Dev receiver `tools/ear_rx.py` + `tools/requirements.txt` (bleak,
  macOS/Linux): pair, GET_CAPS, MIC_OPEN, decode → WAV/live. Select by
  service UUID, never by cached name.
- **Gate:** 10 min streaming: drops < 1 %, heap flat; button-to-audio
  latency ≤ 150 ms; check for BLE-burst noise in the audio (cf. Atom Echo
  amp coupling); measure discharge current while live (120 mAh cell).

### E2.1 — Usability + audio quality pass ✅ DONE (HW) 2026-10-07 (fw 0.4.0)
Prompted by bench use: no way to start pairing, no power-off, A dead unless
a host app was connected, and audio that "sounds bad".
- Pairing window on button B (unpaired Ear doesn't advertise); new hosts
  are refused outside it. Screen legends with arrows point at each button.
- Power key: AXP192 PEK configured + long-press → clean shutdown.
- A works whenever bonded: 3 s ring, fast advertising, delivery on
  reconnect (`ear_rx.py` listen mode is now a reconnecting daemon).
- Audio: PDM x128, -8 dB shelf above 4.5 kHz. Link: DLE 251, msys_1 24,
  GET_CAPS readiness, ring-buffered flow control in place of drop-on-ENOMEM.
- Measured (README "Measured"): Whisper-small WER 3.3–4.3 % (equal to the
  clean source), latency median 43–53 ms (max 83), 0 lost in 5 runs.
- Still owed: power-off and the B/A screens need a human check on HW.
  Passkey pairing was done by the user on 2026-10-07 (macOS) ✅.

### E3 — Genio 720 / NixOS receiver
- `ear-rx` daemon (BlueZ D-Bus GATT client; Python first, C if latency wants):
  find Ear (UUID scan or hive KV), pair once, GET_CAPS, handle AUDIO_START →
  MIC_OPEN, decode → `snd-aloop` playback side; apps capture `hw:Loopback,1`.
- NixOS module: `boot.kernelModules = [ "snd-aloop" ]`, systemd unit,
  reconnect with backoff, bond persistence.
- Verify the Genio's BT controller + BlueZ version: LE SC, DLE, MTU ≥ frame.
- **Gate:** thumb-press on the Stick → audio in an `arecord -D hw:Loopback,1`
  capture on the Genio; survives Ear reboot and Genio reboot without re-pair.

### E4 — Hive integration (consumer-side) + docs
- POSIX `craw_hive` node on the Genio (Duo profile §5 plan) — prerequisite.
- `ear-rx` publishes `ear:<mac4>` per §3.3 from DIS/BAS/ATVV state; Dial dot.
- Docs: README Role 13 Ear (+ topology/projects lists, "accessory" member
  kind); fix stale role list `docs/MagNET-HiveProtocol-v1.md:107`;
  `specs/device-self-registration.md`.
- **Gate:** Dial `kv-list` shows a live `ear:*` record that tracks the
  Ear's state (away → idle → live) as the Stick connects and talks.
- LE SC OOB pairing via the hive is dropped for this board (the Ear can't
  reach the hive); it returns with a WiFi-capable Ear.

### Later (not planned in detail)
- XIAO ESP32-S3 Sense Ear (same components, PSRAM, screenless → OOB pairing).
- LE Audio Ear on BT 5.2 hardware (nRF5340-class) — same hive record,
  `profile:"bap"`.

## 5. Risks

| Risk | Mitigation |
|---|---|
| ~~DRAM~~ — retired by D7: 91.5 KB free with a live link, 77.8 KB largest block. | Remaining levers if E1/E2 need them: NimBLE msys pools, Forth heap 32→24 KB, 8 kHz. |
| ~~WiFi/BLE coexistence~~ — retired by D7 (no WiFi). | — |
| ATVV spec availability / version detail. | E2 prerequisite; Nordic + RDK implementations as cross-reference. |
| The hive record depends on a consumer-side hive client that doesn't exist yet (POSIX `craw_hive`). | E1-E3 don't need it; E4 starts with the port. |
| Genio BT controller quirks (MTU/DLE/LE SC). | Probe early in E2 with `bluetoothctl` + btmon, before writing the daemon. |

## 6. M5StickC Plus gotchas (harvested from Voice_XR / Crawdad)

Treat as unverified: neither project has hardware-validation notes, and the
Voice_XR design-spec is an unedited copy of the Crawdad one.

**Board / toolchain**
- `board = m5stick-c` (no Plus board def); `flash_mode = dio`; 4 MB flash.
- IDF pinned 5.3.1 via `magnet_base` — don't drift to 5.4 (efuse collision,
  `docs/idf-v54-efuse-collision.md`).
- USB serial is a CH552 UART bridge → UART0 console at 115200 (not USB-JTAG).
- M5GFX `>=0.2.0` (locked 0.2.22) auto-detects the board on `display.init()`
  with no args; M5Unified not used.
- Voice_XR runs CPU at 160 MHz; the Ear wants 240.

**Power — AXP192 @0x34 via M5GFX's `lgfx::i2c` port 1 (SDA21/SCL22, 400 kHz)**
- Enable battery ADC: reg 0x82 bit 7. Voltage 0x78-0x79 (12-bit, 1.1 mV/LSB),
  discharge current 0x7A-0x7B (13-bit, 0.5 mA/LSB); charging = reg 0x00 bit 2.
- **Probable bug to not copy:** Voice_XR's battery-% curve applies M5Unified's
  mV thresholds to raw ADC counts — convert to mV first.
- Backlight only via `display.setBrightness()` (M5GFX drives the LDO).
- No power-off/hold/PEK handling exists anywhere — new work if wanted.
- 120 mAh battery: the Ear's runtime with BLE + mic live is a real
  question; measure discharge current in E2.

**Display**
- ST7789V2 135×240; `setRotation(1)` = 240×135 landscape, USB on the right.
- Full-screen `M5Canvas` = 64,800 B heap — the Ear does without it.

**I2C ordering**
- M5GFX owns I2C port 1; anything else on it (AXP192, MPU6886 @0x68) must
  init **after** `display.init()`, via `lgfx::i2c` (no own controller).
- No app-level mutex between I2C users — rely on lgfx locking or serialize.

**Mic (SPM1423 PDM)**
- CLK = **GPIO0** (boot strap pin!) — init I2S only after boot; never load/drive
  it in early init. DATA = GPIO34 (input-only, fine).
- New driver `i2s_pdm` RX, `I2S_NUM_0` master, 16 kHz/16-bit mono, `clk_inv=false`.
  `src/CMakeLists.txt` must REQUIRE `esp_driver_i2s`.
- Settling transient on enable: Voice_XR discards 512 B (~16 ms; comment says
  ~30 ms) — keep the channel running continuously in the Ear, discard once.
- No DC/gain/noise data exists — E1 measures it.
- `craw_mic_peak` has an int16 `-(-32768)` overflow — fix when promoting.

**Buttons / LED / speaker / IR**
- A = GPIO37 (front, thumb → PTT), B = GPIO39 (side). Active-low, external
  pull-ups — GPIO34-39 have no internal pulls (Voice_XR's pull-up calls are no-ops).
  No debounce in Voice_XR: add one for PTT.
- Red LED GPIO10, active-low — use as a hardware "LIVE" indicator.
- Buzzer GPIO2 (LEDC PWM; also a strap pin, keep low at boot). Mind BLE-burst
  coupling into audio (Atom Echo lesson) if chirps are used.
- IR TX on GPIO9 — unused.

**Variants**
- Some units ship SH200Q instead of MPU6886 (irrelevant: the Ear doesn't use the IMU).
- **StickC Plus2 is different** (no AXP192, hold-pin power latch) — this
  project targets the original Plus only; Plus2 would be a separate env.

## 7. Open items

- Obtain the ATVV spec text (Google partner doc) — blocks the E0 profile doc.
- Genio 720 BT chip/attach + BlueZ version on the NixOS image.
- Display art: "a cute ear" icon for the README role roster + idle screen.
