# MagNET Hanasu — ESP-IDF firmware (E-Phase C: full HCP host surface)

**E-C (fw 0.3.0-ec, validated 16/16 on the 4-node bench 2026-07-30)** adds on
top of E-B: `NAME` with on-mesh announce (names in `!CHAT`/`PEERS`, NVS-persisted),
`MODE TERSE|HUMAN`, `SUB`/`UNSUB <classes>` event filtering, machine-readable
`+OK key=value` responses for `STATUS`/`WHOAMI`, `CHANNEL LIST|SHOW` (informational
until E-D), DEGRADED command queueing (max 4 → `+QUEUED n` → `!RESULT` on READY;
code-complete, awaits a DEGRADED window on hardware), and the §4.9 token-bucket
rate limit on host multicast (burst 8, refill 10/s → `-ERR E_RATE_LIMITED`;
verified: 20-burst → 8 accepted, 12 limited). `mn-name!` joins the Forth vocab.

---

Started as the E-Phase A **go/no-go spike** for migrating Hanasu onto ESPIDFORTH
(design proposal §12); now carries the **E-Phase B plaintext MagNET core**:

- v2.1 envelope (16-byte header, §11.1.4) pack/unpack — plaintext, MIC/sig bits reserved
- unified single-role Thread node (fixed dev dataset: channel 24, PAN 0x4d4e, the
  v0.0.6 well-known key — interops on a bench with the Arduino prototype's network)
- CoAP `/magnet` resource + `ff05::abcd` multicast subscribe
- `CHAT <text>` (NON multicast) and `DM <ipv6> <text>` (CON unicast) over HCP;
  `s" hi" mn-chat` / `s" fd..." s" hi" mn-dm` from the Forth REPL
- inbound mesh traffic → `!CHAT` / `!DM` / `!PEER_JOIN` / `!ROLE` / `!STATE` events,
  peer table (`PEERS`), duplicate-drop cache
- **event pump**: OT callbacks post to a queue; a pump task emits. OT context never
  takes the TX mutex (ABBA-deadlock discipline vs. Forth/HCP tasks that hold the TX
  mutex while sending, which takes the OT lock)

Still stubbed until E-Phase D: crypto, passphrase-derived channels, Ed25519 identity
(device_id is currently the EUI-64 tail; the per-sender counter is per-boot random,
so dedup is a recent-cache, not the §11.1.6 monotonic high-water table).

## E-Phase B build result (2026-07-30, esp32c6, IDF 5.3.1)

Builds clean. Flash **798 KB / 2.625 MB (29.0%)**, static RAM **105.6 KB / 320 KB
(32.2%)** (incl. diagnostics vocabulary).

## On-hardware validation (2026-07-30, M5NanoC6, single node) — RAM: **GO**

| Check | Result |
|-------|--------|
| Boot + HCP + Forth alive | ✅ `CAPS`, `@t1 PING` → `@t1 +PONG` (tags work) |
| Thread up, self-promotes | ✅ detached → **leader** ~33 s after boot, `!ROLE leader` → `!STATE READY` |
| **Runtime heap (the go/no-go)** | ✅ **244 KB free** (largest 217 KB, min-ever 243 KB) with OT up + 64 KB Forth heap reserved — 4–6× the ≥40–60 KB target |
| `SELFTEST` | ✅ PASS: env-roundtrip, event-pump, **coap-loopback** (PING to own ML-EID through the full OT/CoAP stack) |
| `BENCH` | env pack ≈1 µs/op, unpack <1 µs/op; TX 8/8 NON multicast, ~162 µs/call |
| `MESH` | role/partition/RLOC16/ML-EID/chan/PAN reported; neighbors: none (single node) |

## Multi-node validation (2026-07-30, 3× M5NanoC6) — **E-B exit test: PASS**

| Check | Result |
|-------|--------|
| 3 nodes attach, exactly one leader | ✅ A leader, B/C routers |
| Multicast chat, every direction | ✅ each node's `CHAT` arrives as `!CHAT` on both others |
| DM by ML-EID | ✅ delivered to target only (third node sees nothing) |
| Peer discovery | ✅ `!PEER_JOIN` + `PEERS` tracks 2 peers per node |
| **Leader failover** (leader physically unplugged) | ✅ survivors re-elect in **~2.5 min** (Thread's ~120 s leader timeout + brief both-claim-leader race resolved by partition merge) |
| Chat after failover | ✅ two-way between survivors |
| Old leader rejoins | ✅ comes back as **router** under the new leader — no competing partition; its chat reaches everyone |

**4th node, XIAO ESP32C6 (2026-07-30): 7/7 PASS** via the `esp32c6_xiao` env
(RF-switch bringup). Joined the running NanoC6 mesh as child → auto-promoted to
router (REED promotion), selftest incl. CoAP loopback passed, chat both
directions with all three NanoC6s, neighbor table shows all 3 routers at
RSSI −14…−38. Also exercised 4-node simultaneous boot (port-open resets all
nodes) → converged to a single leader. Mixed-vendor mesh confirmed.

## Saturation & throughput tests (2026-07-30, 4 nodes: 2× NanoC6 + 2× XIAO)

Method: every node ran `STRESS 300 496` simultaneously — a firmware task
multicasting 496-byte payloads (the 512-byte envelope cap) back-to-back for
5 minutes; `STATS` counters on both sides. 496 B ⇒ each message 6LoWPAN-fragments
into ~5–6 802.15.4 frames, multicast ⇒ **no MAC ACK, no retry**.

**All-4-senders saturation (5 min, 496 B):**

| node | tx attempts | tx accepted | tx rejected | accepted/s | offered | received | loss |
|------|------------:|------------:|------------:|-----------:|--------:|---------:|-----:|
| A (NanoC6) | 159,898 | 3,697 | 156,201 (97.7%) | 12.3 | 5.97 KB/s | 2 | ~100% |
| C (NanoC6) | 161,408 | 4,010 | 157,398 (97.5%) | 13.4 | 6.47 KB/s | 0 | 100% |
| X1 (XIAO) | 144,622 | 3,908 | 140,714 (97.3%) | 13.0 | 6.31 KB/s | 2 | ~100% |
| X2 (XIAO) | 133,581 | 4,213 | 129,368 (96.8%) | 14.0 | 6.80 KB/s | 6 | ~100% |

Aggregate: stack accepted **52.8 msg/s ≈ 25.6 KB/s** offered app payload;
delivered: **10 of 47,484 expected receptions (≈0%)**. This is textbook
congestive collapse for fragmented, unacknowledged multicast: four half-duplex
radios transmitting continuously almost never sit in RX long enough to catch
all ~6 fragments of any one message. Mid-test the mesh partition-flapped
(leader churn at t≈210–270 s — MLE advertisements were starved too) and
**self-healed**; post-test all 4 nodes READY under one leader, peer tables
rebuilding, **zero crashes/WDT, heap min-ever ~238 KB** (≈6 KB dip, no leak)
across 300 s of overload on both board types. `tx rejected` = OT buffer-pool
backpressure (`mn_ot_send` returns error; counted, never blocks).

**Single sender (A bursts, 3 listen, 60 s each):**

| payload | accepted tx | offered | delivered per receiver | delivery rate |
|--------:|------------:|--------:|-----------------------:|--------------:|
| 496 B (≈6 frames) | 16.7 msg/s | 8.07 KB/s | ~8.4 msg/s ≈ **4.05 KB/s** | ~50% |
| 62 B (1 frame) | 48.4 msg/s | 2.93 KB/s | ~21 msg/s ≈ 1.29 KB/s | 39–47% |

**Steady state (paced: 1 msg/s per node, staggered, 5 min per run):**

| payload | sent | delivered | steady-state loss | tx rejects |
|--------:|-----:|----------:|------------------:|-----------:|
| 62 B (1 frame) | 1,200 (3,600 expected rx) | 3,600/3,600 | **0.00%** | 0 |
| 496 B (≈6 frames) | 1,200 (3,039 expected rx) | 2,839/3,039 | **6.6%** (3.4–9.3% per node) | ~4% of sends |

At chat rates the mesh is **lossless** for single-frame multicast — the loss
story is entirely a fragmentation story: one dropped radio frame kills the
whole unacknowledged ~6-fragment message, so large multicast payloads lose
~7% even on a quiet channel (and overlapping fragment trains from two senders
occasionally exhaust OT buffers — the nonzero tx rejects at only 4 msg/s
network-wide).

Takeaways (they confirm §7/§8 and shape §11.6/E-G):
- **Chat traffic is effectively lossless** (0.00% measured at 1 msg/s/node,
  single-frame). Keep interactive messages ≤ ~62 B payload and multicast NON
  is fine without app-layer ACKs.
- **Usable bulk goodput ≈ 4 KB/s per receiver** (multicast, large payloads).
  Large payloads beat small ones for bulk despite fragmentation — per-message
  overhead dominates below ~100 B.
- **Fragmented multicast loses ~7% even at low rate** — fragmentation, not
  congestion, is the loss mechanism. Anything above one frame that matters
  must be CON unicast (or carry Type-4 ACK/NACK recovery).
- Even a single continuous sender loses ~50% on multicast — the radio's own
  TX duty cycle plus no-ACK broadcast. **Reliable transfer must use CON
  unicast (Type 3/6), never multicast** — as the spec already prescribes.
- The ~13 msg/s (496 B) / ~48 msg/s (62 B) per-node acceptance ceiling is the
  OT message-pool backpressure point; app-level rate limiting (§4.9) should sit
  *below* it, and Trickle-style suppression (§11.6) is not optional at scale.
- Chat-scale traffic (single-frame, few msg/s network-wide) sits two orders of
  magnitude below collapse — consistent with the §8 estimate of ~50 casual
  chatters per channel.

Notes for repeating this:
- A software "hold in reset" via RTS **cannot** simulate power-off on the C6 —
  the USB-serial-JTAG bridge is on-die, so dropping EN kills the USB device and
  releases the reset; the chip just power-cycles (~35 s outage, which Thread
  correctly rides out without re-election). Physically unplug the leader.
- Opening a serial port with pyserial toggles DTR/RTS and **resets the node**;
  peer tables are RAM-only, so a fresh `PEERS` after reconnect starts empty
  until traffic arrives.

Two hardware findings, both fixed in this project:

- **4 MB flash-size header** — the `esp32-c6-devkitc-1` board manifest claims 8 MB,
  which stamps an 8 MB image header; on a 4 MB part (M5NanoC6, XIAO C6) IDF's
  startup flash probe asserts and the node **crash-loops**
  (`Detected size(4096k) smaller than ... header(8192k)`). Fixed with
  `board_upload.flash_size = 4MB` in both envs. (This also means the earlier
  "8 MB devkit / OTA" note does NOT apply to the NanoC6 — it is a 4 MB part.)
- **IDF/OpenThread INFO logs bypass the serialized TX writer** and interleave
  mid-line with HCP frames, breaking §11.3 framing for driver use. Fixed with
  `CONFIG_LOG_DEFAULT_LEVEL_WARN=y` in `sdkconfig.defaults.esp32c6` (remember the
  sdkconfig cache: `rm sdkconfig.esp32c6` + clean rebuild for it to take).

## Storage check (do this BEFORE trusting the rest)

ESPIDFORTH ships with `CONFIG_PARTITION_TABLE_SINGLE_APP` → a **1 MB** factory
app. An OpenThread FTD + CoAP + mbedTLS + Forth image is ~1.3–1.6 MB, so the
stock table **cannot hold it**. This project ships a custom `partitions.csv`:

| Partition | Type | Size | Purpose |
|-----------|------|------|---------|
| nvs | data/nvs | 24 KB | Thread dataset + (later) identity key & counter |
| phy_init | data/phy | 4 KB | RF cal |
| factory | app | **2.625 MB** | firmware (≈1.5 MB expected, room to grow) |
| scripts | data/littlefs | 960 KB | persisted Forth autorun scripts (§12.3) |

Total ≈ 3.6 MB on a 4 MB part — fits, **but no OTA** (two app slots don't fit
in 4 MB). For mesh OTA firmware update (Open Q4), move to an **8 MB** C6 module
and a dual-slot table:

```
# 8 MB / OTA-capable
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     ,         0x2000,
phy_init, data, phy,     ,         0x1000,
ota_0,    app,  ota_0,   0x20000,  0x200000,   # 2 MB
ota_1,    app,  ota_1,   ,         0x200000,   # 2 MB
scripts,  data, littlefs,,         0x100000,   # 1 MB
```

**Action item:** confirm whether your C6 devkit is the 4 MB or 8 MB (N8) part
(`esptool.py flash_id`). If OTA matters, standardize on N8 now — it's a
hardware/BOM decision, not a code one.

## Build & flash

```bash
# with OpenThread (the real config — measure this)
pio run -e esp32c6 -t upload -t monitor

# XIAO ESP32C6: use the xiao env — it drives the board's RF switch
# (GPIO3 low = enable, GPIO14 low = built-in antenna) before radio start.
# Without it the XIAO meshes at near-zero range. NanoC6/devkit: plain esp32c6.
pio run -e esp32c6_xiao -t upload

# baseline without OpenThread (footprint comparison)
#   NOTE: for a truly clean baseline also set CONFIG_OPENTHREAD_ENABLED=n,
#   then  rm sdkconfig.esp32c6  and rebuild (PlatformIO caches sdkconfig.<env>).
pio run -e esp32c6_noot
```

The `forth` component is pulled from `../../MagNET_M5DialFiddlerCrab/ESPIDFORTH/components`
via `EXTRA_COMPONENT_DIRS` (not vendored). It requires the `forth_set_io()`
addition made to ESPIDFORTH (`forth_core.{h,cpp}`).

## Build result (2026-06-15, esp32c6, IDF 5.3.1)

Builds clean. Static footprint with OpenThread FTD + mbedTLS + Forth + MagNET:

| Metric | Used | Of | % |
|--------|------|-----|---|
| Flash | 787 KB | 2.625 MB factory | 28.6% |
| Static RAM | 100.7 KB | 320 KB | 30.8% |

Flash is a non-issue. Static RAM leaves ~227 KB; minus the 64 KB Forth heap →
~160 KB for OpenThread runtime + mbedTLS + task stacks. The **static number is
not the verdict** — OpenThread's heavy allocations are at runtime (attach,
message pool, DTLS). The real check is the `# heap[after openthread_start]`
free-RAM line ON HARDWARE.

Two build gotchas were hit and fixed (both in this project, both worth knowing):
- **Don't `set(EXTRA_COMPONENT_DIRS)` in the root CMakeLists** — it clobbers the
  variable PlatformIO uses to map `src/` → the special `main` component, giving
  "Couldn't find the main target of the project!". External components are
  symlinked into `components/` instead (see `components/forth`).
- **`OPENTHREAD_BUILD_DATETIME` mis-quoting** — PlatformIO splits the IDF
  openthread component's space-containing datetime define at the first space,
  breaking every `openthread/*.cpp` with `invalid digit "9" in octal constant`.
  Fixed by `patch_openthread_datetime.py` (pre-build, idempotent).

## Test & diagnostics surface

Every diagnostic exists twice — HCP verb and Forth word — backed by the same C
function (§12.1 one-implementation rule):

| HCP | Forth | What it gives you |
|-----|-------|-------------------|
| `SYSINFO` | `mn-sysinfo` | chip/rev/cores, IDF version, flash size, reset reason, uptime, internal heap (free/largest/min-ever), Forth heap used/free |
| `MESH` | `mn-mesh` | role, partition id, RLOC16, ML-EID, channel/PAN, leader, neighbor table with RSSI/LQI/age |
| `BENCH` | `mn-bench` | envelope pack/unpack µs/op (2000 iters), then 8 NON multicast pings timing the TX call path (needs READY) |
| `SELFTEST` | `mn-selftest` | ① envelope pack→unpack roundtrip ② event-pump: magic PING injected into the queue ③ **CoAP loopback**: magic PING sent to our *own* ML-EID through the full OT/lwIP/CoAP stack and back into `/magnet`. `+OK selftest pass` / `-ERR` |
| `HEARTBEAT <secs>` | `<secs> mn-heartbeat!` | `!HEARTBEAT <state> <uptime> <role> <peers>` cadence (default 30 s, 0 = off; only emits in READY/DEGRADED per §4.6) |
| `STATS [RESET]` | `mn-stats` | tx try/ok/err/bytes + rx msgs/dup/err/bytes since boot or last reset |
| `STRESS <secs> <len>` | `<secs> <len> mn-stress` | saturation burst: a task multicasts `<len>`-byte marked chat frames back-to-back for `<secs>`; receivers count them silently (no `!CHAT` flood); `!STRESS` progress every 30 s, `!STRESS_DONE` with totals |

Quick smoke sequence after flashing (HCP mode):

```
CAPS            → +OK proto=2.1 …
SYSINFO         → # sys … (watch heap free vs. the go/no-go table below)
SELFTEST        → +OK selftest pass       (loopback proves CoAP RX/TX end-to-end, single node)
MESH            → # mesh role=leader …    (alone on the bench, node self-promotes)
BENCH           → # bench env pack=…us/op …
```

Caveat: run `SELFTEST` from HCP mode. At the `ok>` prompt the dispatcher holds
the TX writer across the whole eval, so the pump/loopback stages would stall —
they detect this and report *skipped* (envelope roundtrip still runs).

## Go / No-Go criteria

| Check | Pass condition |
|-------|----------------|
| **Flash fits** | `esp32c6` image links and flashes into the 2.625 MB factory partition with margin |
| **RAM coexists** | After `openthread_start`, `# heap[...]` free is comfortably positive (target ≥ 40–60 KB free, no allocation failures) |
| **Forth alive** | `FORTH` then `2 3 + .` → `5`; `mn-hello` emits the stub chat line |
| **HCP alive** | `CAPS` returns the capability line; `@x1 PING` → `@x1 +PONG` |
| **Thread alive** | `# openthread initialized; entering mainloop` appears and the node stays up (no WDT/heap panic) |

If RAM is the blocker: drop `FORTH_HEAP_SIZE` (main.c) toward 48 KB and/or trim
OpenThread (`OPENTHREAD_CLI=n`, MTD vs FTD). If it still won't fit, that's the
signal to consider an RCP/co-processor split — exactly what this spike exists to
surface early.

## What to record

Paste into the design proposal / issue:
- `firmware.bin` size (esp32c6 vs esp32c6_noot) → OpenThread flash cost
- `# heap[boot]`, `# heap[after forth_init]`, `# heap[after openthread_start]`
- whether Thread actually attached (E-Phase B will form a network; here we only
  init the stack)

## Files

```
firmware-idf/
  platformio.ini          esp32c6 (+ esp32c6_noot baseline)
  CMakeLists.txt          pulls in ESPIDFORTH `forth` via EXTRA_COMPONENT_DIRS
  partitions.csv          custom table (the storage fix)
  sdkconfig.defaults[.esp32c6]   OpenThread on, mbedTLS PBKDF2/HKDF/CCM, USB-JTAG console
  src/main.c              bringup in §12.5 order
  components/magnet/
    include/magnet.h           core API (shared by HCP + Forth)
    include/magnet_envelope.h  v2.1 envelope (§11.1.4)
    magnet_envelope.c          envelope pack/unpack (plaintext, E-B)
    magnet_core.c              state + serialized TX writer + event pump + peers + chat/dm
    magnet_link.c              dual-mode HCP/Forth dispatcher (CHAT, DM, PEERS, …)
    magnet_forth.c             mn-* FFI vocabulary (mn-chat, mn-dm, mn-peers, …)
    magnet_ot.c                Thread bringup + CoAP /magnet (gated by MN_ENABLE_OPENTHREAD)
```

## Known caveats (E-B level)

- **On-hardware validation pending** — the E-B exit test (3-node failover +
  multicast chat) and the runtime heap go/no-go have not run yet; the
  `esp_openthread_*` init sequence follows the canonical example shape for
  IDF 5.3.1 and may need minor field tweaks on first flash.
- **Plaintext only** — `Flags.ENCRYPTED` frames are dropped on RX; crypto,
  passphrase channels, and identity land in E-Phase D. The channel selector is a
  fixed constant (`0x6d61`) until derived from `root_secret`.
- **Dedup is a 32-entry recent-cache**, not the §11.1.6 monotonic table — the
  per-sender counter starts at a per-boot random value (no NVS persistence yet).
- **`mn-chat` from the REPL** uses `s"` (added to the engine), e.g.
  `s" hello team" mn-chat`. `mn-hello` (no args) remains a zero-dependency smoke
  test. `s"` interpret-mode strings live in a 4-deep rotating transient buffer
  (fine for same-line use); compiled `s"` strings are permanent.
- No echo in FORTH mode (raw USB-JTAG). Fine for scripted/LLM use; add local
  echo if a human finds it awkward.
- If CMakeLists source-list edits seem ignored (undefined references to freshly
  added files), delete `.pio/build/<env>` — the cached CMake configure does not
  always re-run.
