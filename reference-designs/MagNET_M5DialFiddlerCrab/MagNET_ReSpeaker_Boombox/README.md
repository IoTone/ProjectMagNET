# MagNET ReSpeaker Boombox (Role 12)

Hive node implementing **Role 12: Boombox** — *"any speakers or audio playback device. Different than the beeper, it can play full sounds, music, or recordings."*

Built on a Seeed XIAO ESP32-S3 socketed onto the [ReSpeaker Lite Voice Kit](https://wiki.seeedstudio.com/reSpeaker_usb_v3/) carrier. The mic-array side of the carrier is unused in v1 — this firmware is output-only. A future Role 11 (Eye-with-ears) can consume the input chain.

## Hardware

| | |
|---|---|
| Base | XIAO ESP32-S3 + Seeed [ReSpeaker Lite Voice Kit](https://wiki.seeedstudio.com/reSpeaker_usb_v3/) |
| MCU | ESP32-S3, 8 MB flash, no PSRAM |
| Codec | TI TLV320AIC3204 (I2C addr `0x18`) — wakes itself via the on-carrier XU316 |
| DSP/router | XMOS XU316 (I2C addr `0x42`) — generates I2S BCLK/WS, routes audio to speaker |
| Audio out | I2S0 **slave** → XU316 → TLV320AIC3204 → speaker (or 3.5 mm jack overrides) |
| Console | USB-C native serial-JTAG |

### Pin map

Confirmed against `xiao_esp32s3_arduino_examples/xiao_i2c_control_volume.ino` from the official [ReSpeaker_Lite repo](https://github.com/respeaker/ReSpeaker_Lite). The XIAO is socketed, so reprogramming is one edit at the top of `src/main.c` if a future carrier rev moves these.

| Signal | XIAO GPIO | Direction (from XIAO) | Notes |
|--------|-----------|-----------------------|-------|
| I2S BCLK | 8 | input (slave) | XU316 drives the bit clock |
| I2S WS / LRCK | 7 | input (slave) | XU316 drives the word select |
| I2S DOUT | 43 | output | speaker-bound stream into XU316/codec |
| I2S DIN | 44 | (unused in v1) | mic-array data, would belong to Role 11 |
| I2C SDA | 5 (D4) | bidir | for codec/XU316 register access |
| I2C SCL | 6 (D5) | bidir | for codec/XU316 register access |

The ESP32-S3 acts as **I2S slave** on the TX side — the XU316 is the master and generates clocks. Slot format is **stereo 32-bit** even though the synth produces mono 16-bit; the render task expands `int16 → int32 (sign-extended into upper bits)` and duplicates across L/R before each I2S write.

## Audio architecture

- **16 kHz / 16-bit synth → 32-bit stereo I2S** at slave-mode 16 kHz frame rate.
- **Software synth** with a 256-entry sine LUT (linear interpolation, Q16.16 phase) — `craw_audio_synth.c`.
- **Render task** at priority 5 drains a FreeRTOS queue of play requests; each request is up to 32 segments. Per-segment kinds: `TONE`, `SWEEP`, `AM`, `SLEEP`. Each segment can carry a **gain envelope** (`gain` → `gain_end`) to avoid click artifacts and to compose ADSR-style fades.
- **No PCM data on disk.** Patterns are static-const `craw_audio_seg_t[]` arrays in flash rodata; adding a notification = one new array + one play function. Bundle authors get the same primitives without any C code at all.

```
Forth REPL  ──┐                                        ┌── XU316 ─┐
              ├─→ [request queue] ─→ render task ─→ I2S0 ─┤        ├─→ codec ─→ speaker
KV cmd poll ──┘    (16 deep)        (sine LUT)             └── XMOS ─┘
```

# Forth reference

Every audio command is a Forth word. Stack effects use the standard `( before -- after )` notation. Words are grouped by category.

## Synth primitives

These are the building blocks. The five canned recipes are composed from these, and so is anything you'd add via a role bundle.

---

### `tone` ( freq dur -- )

Plays a steady sine at `freq` Hz for `dur` ms. Uses a default per-segment gain of 0.7 (further scaled by master `vol`). Returns immediately — the request is enqueued and the render task plays it asynchronously.

```forth
1500 200 tone     \ short beep at 1.5 kHz
262  500 tone     \ middle C for half a second
800  60  tone 60 sleep
800  60  tone     \ two short bursts with a gap (manual ding)
```

Stack: takes two cells (frequency in Hz, duration in ms), produces nothing.
Range: any frequency below 4 kHz is safe at 16 kHz sample rate; above 4 kHz aliases.
Duration: positive integer ms; zero is silently dropped.

---

### `sweep` ( f0 f1 dur -- )

Linear frequency sweep from `f0` Hz to `f1` Hz over `dur` ms. Direction is implied by the order — `400 1600 600 sweep` rises, `1600 400 600 sweep` falls. Same default gain as `tone`.

```forth
400  1600 600 sweep    \ rising chirp (alert-style)
1600 400  600 sweep    \ falling chirp (error-style)
800  800  300 sweep    \ degenerates to a 300 ms tone (f0 == f1)
```

Frequency interpolation is linear in time, not exponential — the perceived pitch climb is faster at low frequencies and slower at high ones (because human pitch perception is logarithmic). For an "even-feeling" sweep across a wide range, prefer chained shorter sweeps.

---

### `am` ( fc fm dur -- )

Sine carrier at `fc` Hz amplitude-modulated by a sine at `fm` Hz, for `dur` ms. Used for siren / wobble effects and is the basis of the `warn` recipe.

```forth
1000 4   800 am   \ 1 kHz tone wobbling 4 times per second (warn)
2000 12  500 am   \ faster wobble — angrier alarm
500  20  300 am   \ low buzz
```

Modulation depth is fixed at 100% (output ranges 0..gain). Carrier and modulator phases are tracked independently, so the wobble is consistent across long durations.

Practical `fm` range: 1–20 Hz. Above ~30 Hz the modulator becomes audible as a separate tone (you're entering ring-modulation territory).

---

### `sleep` ( ms -- )

Inserts a silence segment of `ms` ms. Useful as a spacer inside multi-segment compositions or to hold a gap before the next play request.

```forth
800 60 tone 100 sleep 800 60 tone     \ two-beep spacing
1500 200 tone 1000 sleep alert        \ pause, then alert sequence
```

Stack: one cell (duration in ms).

## Notification recipes

Built-in patterns composed from the four primitives. Each is a single Forth word with no stack effect — fire-and-forget.

| Word        | What it sounds like                                              | Duration |
|-------------|------------------------------------------------------------------|----------|
| `alert`     | Three rising chirps 600 → 1200 Hz, 80 ms gaps                    | ~1.0 s   |
| `notify`    | Two-beep "ding-ding" at 1500 Hz with attack/release envelopes    | ~360 ms  |
| `warn`      | 1 kHz carrier amplitude-modulated at 4 Hz (alarm beep)           | ~840 ms  |
| `error`     | Descending sweep 800 → 350 Hz + 200 ms hold                      | ~600 ms  |
| `siren`     | Two-cycle wail 500 ↔ 1500 Hz, 600 ms per sweep                   | ~2.4 s   |
| `yelp`      | Fast wail variant — three cycles at 200 ms per sweep             | ~1.2 s   |
| `nee-naw`   | European two-tone alarm — alternating 950 Hz / 750 Hz, 400 ms each | ~2.4 s |
| `air-raid`  | Slow ominous 300 → 800 Hz rise, 800 ms hold, slow fall back to 300 Hz | ~3.8 s |
| `sunrise`   | Ascending C-major arpeggio (C4-E4-G4-C5) with crescendo + fade   | ~1.4 s   |

`sunrise` plays once at boot when audio is up. The other eight only play on demand.

### When to pick which siren

- **`siren`** — generic emergency wail. The default if you just need "siren-y."
- **`yelp`** — fast urgent feel. Good for "I need attention right now" without the ~2.4 s wait of a full `siren`.
- **`nee-naw`** — distinctly European-coded; reads as "fire engine" or "police" to anyone familiar with those sounds. Use when you want unmistakable "alarm/dispatch" character (same length as `siren` but melodic, not wailing).
- **`air-raid`** — the longest (~3.8 s) and the most ominous; rise + hold + fall. Reserve for "this is a big deal" events (catastrophic error, evacuation drill, intentional theatrical effect). Don't use casually — it occupies the speaker for almost four seconds and there's no overlap mixing.

`warn` is the odd one out — it's an *alarm beep* (fast amplitude pulsing at fixed pitch), not a wailing siren. Pair `warn` with brief contextual events; pair `siren` family with sustained ones.

### Recipe definitions, for cross-reference

These match `components/craw_audio/craw_audio_patterns.c`. Annotated as Forth-equivalent so you can copy-paste and tweak.

```forth
\ alert — three rising chirps with envelope-in/out on the outer ones
: alert
   600 1200 200 sweep   \ envelope 0.0 → 0.7 implicitly
    80      sleep
   600 1200 200 sweep
    80      sleep
   600 1200 200 sweep ; \ envelope 0.7 → 0.0

\ notify — attack-release envelope on each ding
: notify
   1500 80 tone   1500 20 tone
    100     sleep
   1500 80 tone   1500 20 tone ;

\ warn — single AM segment with attack and release tail
: warn
   1000 4 800 am
   1000 4  40 am ;

\ error — descending sweep + held low
: error
   800 350 400 sweep
   350     200 tone ;

\ siren — two wail cycles with envelope on the outer sweeps
: siren
   500  1500 600 sweep
   1500 500  600 sweep
   500  1500 600 sweep
   1500 500  600 sweep ;

\ sunrise — C major arpeggio with crescendo
: sunrise
   262 240 tone     \ C4
   330 220 tone     \ E4
   392 220 tone     \ G4
   523 700 tone ;   \ C5 (longest, fades out)
```

The C versions get **per-segment gain envelopes** (start gain → end gain) which the basic Forth `tone`/`sweep`/`am` words don't expose — bundles can call `craw_audio_play_pattern()` directly via FFI if envelope shaping is needed. v2 may add `tone-env ( freq dur g0 g1 -- )` etc. for envelope-aware Forth words.

## Volume + amp control

### `vol` ( n -- )

Sets master volume to `n` (0..100). Persisted in NVS namespace `boombox` key `vol`, so it survives reboots. Affects all subsequent playbacks; in-flight pattern is not retroactively rescaled.

```forth
30 vol     \ quiet
70 vol     \ medium-loud
100 vol    \ max
```

### `vol?` ( -- n )

Pushes the current volume onto the stack and prints it.

```forth
vol?
\ → volume: 60
\   60 left on the stack
```

### `audio-on` ( -- )

Enables the amplifier output stage. If the carrier exposes an amp-enable GPIO (it currently doesn't on the ReSpeaker Lite Voice Kit, so this is a no-op), this drives that pin active. Audio still synthesizes whether amp is on or off — only the speaker output changes.

### `audio-off` ( -- )

Disables the amp. Same caveat — currently a no-op for ReSpeaker Lite Voice Kit, but reserved for future carriers that expose a mute pin.

### `audio-stop` ( -- )

Aborts the currently rendering segment and drains the request queue. Silence resumes within ~32 ms (one DMA buffer at 16 kHz). Useful to cut off a long pattern early — `siren` runs ~2.4 s, `audio-stop` interrupts it on the next chunk.

```forth
siren
\ ... 500ms later ...
audio-stop    \ silence within 32ms
```

### `audio-status` ( -- )

Prints the audio subsystem state. Read this first when troubleshooting.

```
amp:       on
volume:    60
rendering: yes
queue:     2 / 16
segments:  47
samples:   1245888
```

| Field      | Meaning                                                               |
|------------|-----------------------------------------------------------------------|
| `amp`      | Whether the amp gate is currently enabled (no-op on Lite Voice Kit).  |
| `volume`   | Current master volume 0..100.                                         |
| `rendering`| `yes` if the render task is currently producing samples.              |
| `queue`    | Pending play requests / queue depth.                                  |
| `segments` | Lifetime count of segments rendered. Should monotonically increase.   |
| `samples`  | Lifetime sample count. Useful to confirm the synth is actually feeding the I2S — if `siren` doesn't make sound, watch this number; if it climbs, the synth is fine and the problem is downstream (carrier/codec). |

## Hive + provisioning

These mirror every other MagNET node project — same words, same behavior. See the top-level README for details on the hive protocol.

### `prov-status` ( -- )

Print BLE + WiFi + IP + hostname + hive state. Read this first to understand bringup state.

```
ble:    MagNET-biologic-XXXX
wifi:   connected
ssid:   MyHomeNetwork
ip:     192.168.1.42
time:   synced
host:   magnet-boombox-XXXX.local
hive:   joined
```

### `prov-reset` ( -- )

Clears stored Wi-Fi credentials from NVS and reboots into BLE provisioning mode. Use to switch networks or recover from a wedged WiFi state.

### `hive-status` ( -- )

Prints the hive node state machine and current session id.

```
hive:    JOINED
node:    MagNET-biologic-XXXX
session: 7f9a0c1d-...
```

### `kv-get` ( -- )

Interactive: prompts for a key, fetches it from the ruler's KV table over the hive session, prints the value. Blocks for up to 3 s.

```
boombox> kv-get
key: bridge:status
'bridge:status' = 'MagNET-biologic-b7a4'
```

### `kv-put` ( -- )

Interactive: prompts for key + value, writes to the ruler's KV table. Fire-and-forget — no ACK in the v1 protocol.

```
boombox> kv-put
key:   note:scratch
value: hello from boombox
kv-put rc=0
```

# Hive integration

Beyond the standard role + bundle install pipeline, the boombox exposes two hooks for cross-node audio control.

## `boombox:cmd` — remote command channel

Every 2 s while joined, the boombox does `KV_GET boombox:cmd`. If the value is non-empty, it's evaluated as a Forth phrase, then the key is cleared. So **any other peer can trigger any boombox sound** via the existing KV protocol — no new wire format, no pub/sub.

From the Dial REPL:
```forth
s" alert" s" boombox:cmd" kv-set       \ play alert on the boombox
s" siren" s" boombox:cmd" kv-set       \ siren
s" 1500 100 tone" s" boombox:cmd" kv-set    \ freeform Forth
```

The phrase is `forth_eval`'d in the boombox's Forth context, so anything in its dictionary is reachable — including bundle-installed words like `sos` or `fanfare`. Failed evals print to the boombox's serial console.

A consequence worth knowing: this is fire-and-forget. If two peers race to write `boombox:cmd` within the same 2 s window, only one wins. For coordinated playback across multiple boomboxes (when there are multiple — there aren't yet), the v2 plan adds a per-node command key `boombox:cmd:<mac4>`.

## `boombox:status` — heartbeat for indicators

Every 5 s while joined, the boombox publishes:

```
boombox:status = MagNET-biologic-XXXX:<vol>:<amp_state>
```

A future Dial indicator dot (mirroring the existing `bridge:status` / `redis:status` dots) can read this KV and light when a boombox is reachable. Not yet wired in `M5StackDial-m5gfx-demo-ESPIDFORTH/src/main.cpp` — bookmark for later.

# Cookbook

Recipes that don't ship in firmware but are easy to add via a role bundle. Each example is one bundle's source.

## SOS in Morse code

```forth
: dot   800 60  tone  60 sleep ;
: dash  800 180 tone  60 sleep ;
: sos   dot dot dot   100 sleep
        dash dash dash 100 sleep
        dot dot dot ;
```

After `bundle-install`, `sos` is a permanent word — survives reboots via `craw_role_bundle`'s NVS persistence.

## Siren-style sounds (`yelp`, `nee-naw`, `air-raid`)

These three are now built-in canned recipes — see the table above. The Forth-equivalent decompositions are below for reference; reproduce them in a bundle if you want to override the C versions or tweak parameters.

```forth
\ yelp — three full cycles, 200 ms per sweep, ~1.2 s
: yelp1   500 1500 200 sweep   1500 500 200 sweep ;
: yelp    yelp1 yelp1 yelp1 ;

\ nee-naw — European two-tone, 400 ms per tone, three cycles, ~2.4 s
: nee     950 400 tone ;
: naw     750 400 tone ;
: nee-naw nee naw nee naw nee naw ;

\ air-raid — slow rise + hold + slow fall, ~3.8 s
: air-raid
   300 800 1500 sweep
   800     800 tone
   800 300 1500 sweep ;
```

The C versions add per-segment **gain envelopes** for clean attack/release; the pure-Forth versions skip envelope shaping (since `tone`/`sweep` don't expose `gain_end` yet). On a small speaker the difference is subtle but the C versions click less at start/stop.

## Boot fanfare with retry on failure

```forth
: \ Drop this in a bundle, register a custom on-boot hook.
  fanfare  523 200 tone  659 200 tone  784 400 tone ;

: try-with-fanfare ( xt -- result )
  execute  \ run the supplied word
  dup 0 = if fanfare else error then ;
```

## Doorbell-style chime

```forth
: ding  784 200 tone ;     \ G5
: dong  587 350 tone ;     \ D5
: chime ding 100 sleep dong ;
```

# Smoke test

1. Flash; reboot.
2. Boot banner shows `MagNET gen 0.5.0-spore` and `[audio] I2S0 ready`.
3. **Sunrise plays** automatically right before the REPL prompt.
4. REPL: `1500 200 tone` — single short beep, clean sine.
5. REPL: `400 1600 600 sweep` — rising chirp.
6. REPL: `siren` — two-cycle wail. Distinct from `warn`.
7. REPL: cycle through `alert` / `notify` / `warn` / `error`.
8. REPL: `30 vol` — reduce volume; `notify` should be quieter.
9. REPL: `audio-status` — confirm `samples` increases each playback.
10. Power-cycle. Volume preserved. Boot is silent through hardware/WiFi/BLE init, then sunrise.
11. Provision Wi-Fi via BLE (nRF Connect) if not already; confirm `hive-status` JOINED.
12. From the Dial REPL: `s" alert" s" boombox:cmd" kv-set`. Within ~2 s the boombox plays `alert` without you touching its console.
13. Author + push a bundle (`scripts/sign_bundle.py` from `bundles/`); confirm the new word becomes permanent.

# Troubleshooting

| Symptom | Most likely cause | Recovery |
|--------|---|---|
| Silent (sunrise doesn't play, no error) | I2S not getting clocks. Check `audio-status`: `rendering: yes` but `samples` stuck means the XU316 isn't producing BCLK/WS — usually means the carrier didn't power up the codec rail. | Power-cycle the carrier. If stuck, flash a known-good Seeed Arduino example and confirm hardware. |
| White noise instead of music | Wrong I2S role / pins / format. ESP32-S3 must be **slave**, pins must be BCLK=8 WS=7 DOUT=43, format must be 32-bit stereo slots. | The current firmware is configured this way; if you forked it, double-check `i2s_setup()` in `craw_audio_i2s.c`. |
| Distorted but recognizable | Format mismatch — Philips vs MSB, or bit-width misalignment. | Confirm `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG` is in use. |
| Audio cuts in/out | Render task starving. Other high-priority work blocking — common when MQTT bridge or Redis sidecar runs alongside. | Check task priorities; render task is prio 5. If you've added higher-prio tasks, either reduce them or bump render to 6. |
| `boombox:cmd` doesn't trigger | Hive session not active or KV poll task crashed. | `hive-status` should say JOINED; if not, fix WiFi first. If JOINED and still no response, check the boombox serial for `[cmd]` log lines on each tick. |

# NVS storage

| Namespace | Key  | Type | Default | Notes |
|-----------|------|------|---------|-------|
| `boombox` | `vol`| u8   | 60      | Master volume 0..100, persisted on every `vol` write. |
| (shared)  | `wifi/*` | str | (none) | WiFi credentials managed by `craw_nvs`. |
| (shared)  | role bundles | blob | (none) | Installed Forth role bundles managed by `craw_role_bundle`. |

# Known limits / v2 candidates

- **Mic input** unused. Future Eye-with-ears role would need an XU316 host-mode init step (~200 lines of UART/I2C config).
- **Sample rate** fixed at 16 kHz — fine for notifications, not for music. Bumping to 44.1 kHz is one constant + 2× DMA buffer.
- **No PCM file playback.** v2 could add a WAV/CAF reader pulling from NVS or SD; envelope shape stays compatible.
- **Single render task.** No layering — a new request waits for the current pattern to finish (or be `audio-stop`'d). Mixing two streams would need a per-stream phase + an output mixer.
- **No envelope-aware Forth words.** `tone-env` / `sweep-env` etc. would let bundles compose ADSR-shaped sounds without dropping to C. Easy to add when needed.
- **No volume-curve options.** Master volume is linear; for "perceptually equal" steps a logarithmic curve would be friendlier (small step at low volume = audible change; small step at high volume = barely audible).
- **`audio-on` / `audio-off`** are placeholders on this carrier (no amp gate exposed). Carriers that do expose one will work as-is by setting `AMP_PWR_EN_GPIO`.
- **No "boombox cap" enforcement** — the role bundle just trusts caller-declared caps for now; matches existing role-bundle pattern.

# Related work

- **`M5Capsule_Hive_Scribe/`** — bringup pattern (BLE/WiFi/SNTP/hive join order, role-bundle install pipeline) is copied verbatim.
- **`M5StackDial-m5gfx-demo-ESPIDFORTH/`** — speaker uses LEDC PWM driving a piezo, totally different architecture; the Boombox uses I2S into a real codec for richer output.
- **`FiddlerWAIch/Resources/Sounds/` (planned)** — the chirp_working/finished/needinput/error vocabulary. If we ever want the watch and the Boombox to "speak the same language" of notifications, the recipes in `craw_audio_patterns.c` are the reference.
- **ReSpeaker Lite Voice Kit wiki**: <https://wiki.seeedstudio.com/reSpeaker_usb_v3/>
- **Seeed reference repo**: <https://github.com/respeaker/ReSpeaker_Lite> — the `xiao_esp32s3_arduino_examples/` folder has the canonical pin map and codec interaction examples.
- **TLV320AIC3204 datasheet** — for v2 codec-direct register access (lineout gain, headphone routing, mic preamp). Currently the XU316 mediates everything.
