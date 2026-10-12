# M5StickCPlus_Hive_Ear — MagNET Hive Role 13: Ear

A C-3PO-style **thumb mic**: hold button A, talk, and the paired host hears
it. The Ear is a **BLE-only hive accessory**: a standard **Android TV
Voice-over-BLE (ATVV) v1.0** remote, with no WiFi and nothing
MagNET-specific on air. Its bonded consumer (e.g. a Genio 720 on NixOS)
reads it and publishes its hive record. Background: `docs/PLAN.md`. Wire
contract: `docs/Ear-ATVV-profile.md`.

Hardware: **M5StickC Plus** (original, AXP192, ESP32-PICO-D4, no PSRAM).
Not the Plus2. The **SE** has an identical SoC and should work as-is (its
missing IMU isn't used).

## Build / flash

```bash
pio run -e m5stickc_plus -t upload --upload-port /dev/cu.usbserial-XXXX
pio device monitor -p /dev/cu.usbserial-XXXX      # Forth REPL, 115200
```

Opening the CH552 serial port resets the Stick.

## Use

The screen labels each button next to the button itself: a bar along the top edge (▲) for **B**, a bar at the bottom right (▶) for the front button **A**. A label only appears when that button does something right now. macOS System Settings → Bluetooth never lists the Ear; pair from `tools/ear_rx.py` (or nRF Connect).

| Action | What happens |
|---|---|
| Power on, never paired | Screen: **NOT PAIRED / B: pair**. The Ear is invisible (not advertising) |
| Press **B** (side) | Opens a 2-minute **pairing window** (screen: `PAIRING m:ss`, advertises as `MagNET-Ear-XXXX`). Press again to cancel |
| Host connects in the window | Screen shows a 6-digit **passkey**; type it on the host. Then **PAIRED** |
| Power on, paired | Screen: **PAIRED / hold A: talk**; advertises so bonded hosts can reconnect. New hosts are refused unless B is pressed first |
| Host app connected | Screen: **READY / hold A: talk** |
| Hold **A** (front), host connected | `AUDIO_START` (hold-to-talk), red LED + **TALKING**, 16 kHz audio |
| Hold **A**, no host connected | **CALLING HOST**: the Ear records into a 3 s buffer and advertises fast. When a bonded host app reconnects it gets the whole utterance (**SENDING**), faster than real time. No host within 6 s: **NO HOST** |
| Release **A** | `AUDIO_STOP` (button release), after any buffered audio is sent |
| Host `MIC_OPEN` | **MIC OPEN**; streams until `MIC_CLOSE`, or 15 s without `MIC_EXTEND` |
| Hold **B** 5 s | Countdown `FORGET ALL? n`, then forget all bonds and reboot. Release early to cancel |
| Hold **power key** 1.5 s | **POWER OFF** (the small button on the bottom edge). Press it to turn back on. If the firmware is hung, holding it 6 s cuts power in hardware |

Forth words: `ble-pair` (same as pressing B), `ear-stat`, `ear-mem`, `mic-stat`, `N mic-rec` + `mic-dump`
(→ `tools/ear_dump.py log out.wav`), `N ear-gain` (AGC ceiling, persisted),
`ear-gain?`, `ear-selftest`, `N talk` (simulated button hold), `ble-unpair`,
`power-off`, `N axp@` (read an AXP192 register).

## Hosts

**Dev / any OS: `tools/ear_rx.py`** (bleak; `pip install -r tools/requirements.txt`)

```bash
tools/ear_rx.py                   # host daemon: reconnects forever; each hold -> ear_NNN.wav
tools/ear_rx.py --open 5          # host-initiated 5 s capture
```

First time: press **B** on the Ear, then run the tool. macOS asks for the
passkey shown on the Stick (and, once, for Terminal's Bluetooth permission).
If macOS remembers an older pairing with the Ear, forget it in System
Settings → Bluetooth first.

**Linux without PipeWire (Genio 720 / NixOS): feed an ALSA loopback**

```bash
# once: press B on the Ear, then bluetoothctl → agent KeyboardDisplay → scan on → pair <addr> (type passkey) → trust <addr>
tools/ear_rx.py --pcm - | aplay -q -t raw -f S16_LE -r 16000 -c 1 -D hw:Loopback,0,0
# consumers record the Ear as an ordinary mic:
arecord -D hw:Loopback,1,0 -f S16_LE -r 16000 -c 1 speech.wav
```

NixOS sketch (E3, untested on the Genio):

```nix
boot.kernelModules = [ "snd-aloop" ];
hardware.bluetooth.enable = true;
systemd.services.magnet-ear = {
  wantedBy = [ "multi-user.target" ];
  after = [ "bluetooth.service" "sound.target" ];
  serviceConfig.Restart = "always";
  path = [ pkgs.alsa-utils (pkgs.python3.withPackages (p: [ p.bleak ])) ];
  script = "ear_rx.py --pcm - | aplay -q -t raw -f S16_LE -r 16000 -c 1 -D hw:Loopback,0,0";
};
```

**Linux with PipeWire:** [ATVVoice](https://github.com/zhanglongqi/ATVVoice)
(MIT, Rust, ships a `flake.nix`) is an off-the-shelf ATVV v1.0 host that
exposes the remote as a PipeWire microphone. The Ear speaks the protocol it
implements.

## Measured (HW, 2026-10-07, fw 0.4.0, Mac host)

End-to-end test: the Mac speaks 11 Harvard/command sentences (92 words) and
timed clicks through its speaker into the Ear (~30 cm), with MIC_EXTEND
every 1 s. Five runs on the final build:

- **Speech-to-text:** Whisper `small` word error rate **3.3–4.3 %** through
  the Ear, vs 3.3 % on the clean TTS source. The Ear adds no measurable
  errors at this size. (`base` 12 %, `tiny` 8.7 % on an earlier capture: use
  `small` or larger.) A fresh button-A press with no host connected came
  out word-for-word.
- **Latency** (speaker sound → decoded frame in the host process): median
  **43–53 ms**, range 32–83 ms. Budget: 16 ms frame + ~10 ms I2S DMA +
  0–30 ms waiting for the connection event (macOS grants 30 ms) + a few ms
  in the host stack. Each stream has a one-off ~96 ms mic start-up before its
  first frame (gotcha 4). A Linux host can request a 7.5–15 ms interval.
- **Reliability:** 0 lost / 0 dropped frames in every run since the ring
  buffer and DLE; host writes never fail.
- **No-host talk press:** the Mac reconnected in ~2 s; the 3.9 s utterance
  arrived intact in 1.8 s.
- **Memory:** 48 KB internal heap free idle (the 3 s audio ring costs 25 KB).

Earlier (2026-10-06, fw 0.3.0):

- **Memory:** 76.8 KB internal heap free while bonded and streaming; flat
  across streams.
- **MIC_OPEN, 6 s:** 376 frames, 0 lost / 0 dropped. A 1 kHz test tone and
  speech were recovered intact on the host, with 0 clipped samples.
- **Hold-to-talk:** two holds, 245 + 183 frames, 0 lost, clean
  `AUDIO_STOP 0x02`.
- **Codec self-test:** 32.3 dB round-trip SNR.
- **Mic, after the fixes below:** sub-60 Hz energy 0.1 %. Speech window
  90 % in 150 Hz-4 kHz.

## StickC Plus gotchas found on HW

1. **Mic power:** the SPM1423 is fed by **AXP192 GPIO0 run as an LDO**
   (`0x91=0xF0`, `0x90=0x02`). M5GFX doesn't enable it. Without it, PCM
   reads a constant ~-30935.
2. **PDM slot:** the mic answers on the **RIGHT** slot. IDF's mono default
   is LEFT. Fixed in the shared `craw_mic` 0.2.0, which also provides the
   high-pass (`craw_mic_hpf_*`) used by both the Ear and Voice_XR.
3. **PDM DC drift:** the classic ESP32 has no PDM high-pass filter. A big DC
   offset wanders below 60 Hz and buried speech until a 120 Hz biquad was
   added.
4. **PDM start-up transient:** after the channel is enabled, output starts
   at -32768 and takes ~80 ms to reach its DC level. `craw_mic` now discards
   96 ms (it used to discard 16 ms, which pinned one-shot peaks at 32767).
   This adds ~96 ms before the first audio frame of each stream.
5. **NimBLE INFO logs every notify:** about 125 lines/s at 115200 baud.
   Keep NimBLE logging at WARNING.
6. **PDM oversampling:** IDF's default is x64. x128 (2.048 MHz PDM clock)
   cut Whisper errors (5.4 → 2.2 % on one A/B pair). `craw_mic` now defaults
   to x128.
7. **Bright mic:** in this case the SPM1423 reads 6–10 dB hot above 4 kHz,
   heard as hiss and harshness. A -8 dB high shelf at 4.5 kHz
   (`ear_audio.c`) restores the source's tilt within ~2 dB.
8. **No Data Length Extension by default:** each 131-byte notification went
   out as 5 × 27-byte radio packets. On a busy Mac radio that sometimes
   allowed ~35 frames/s against the 62.5 needed, so audio queued up to 0.5–1
   s or dropped. `ble_gap_set_data_len(251)` on connect fixed it.
9. **NimBLE msys_1 pool:** at the default 12 blocks, notifications drained
   it and host writes failed with "Insufficient Resource". Now 24, and the
   Ear keeps 8 free.
10. **Restored subscriptions ≠ host ready:** a bonded host's notification
    subscriptions come back at encryption, before its app is listening.
    The Ear now waits for GET_CAPS on each connection before it streams.
11. **Power key:** nothing configured the AXP192 power key (M5GFX doesn't),
    so it did nothing. The Ear now sets `0x36=0x5D` (1.5 s long press,
    6 s hardware off) and polls the long-press IRQ (`0x46` bit 0) to call a
    clean `0x32` bit-7 shutdown.

## Bench-only build flag

`PLATFORMIO_BUILD_FLAGS=-DEAR_DEV_JUSTWORKS=1` switches to Just Works pairing:
encrypted, but **no MITM protection**, and the screen says `DEV: NO MITM`.
It exists for unattended bench tests only. Never put it in `platformio.ini`.
