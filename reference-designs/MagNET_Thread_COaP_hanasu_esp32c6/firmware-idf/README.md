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

# BLE provisioning build: advertises until a channel is set, then tears BLE down
pio run -e esp32c6_ble -t upload

# Companion node (magnet_app SCOPE §1 Option A, decided 2026-08-02): BLE stays
# resident so the phone app can attach as its live window into the mesh.
# Flash exactly ONE bench node with this — it re-opens the bonding window on
# every boot, so it's a diagnostic anchor, not a fielded node.
pio run -e esp32c6_ble_resident -t upload

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

## BLE pairing

Bonding is subtle and cost a lot of cycles to get right. Before touching
`magnet_ble.c`'s security configuration or the app's bonding path, read
**`../docs/BLE-PAIRING.md`** — it records what is established (with evidence)
and what is still unverified on both the NimBLE and Android sides.

The single most important point: **only the OS pairing prompt can complete a
bond**; no in-app affordance can substitute for it.

## Bot mode (test builds only) — validated 2026-08-04

`-DMN_ENABLE_BOTS=1`, env **`esp32c6_ble_resident_bots`**, compiles in an
opt-in auto-responder: `BOTMODE 0` (HCP) or `0 botmode` (Forth) makes a node
answer inbound chat with a one-line proof of life. Off at every boot, never
persisted, and compiled out of every other env — so the soaked images stay
bit-identical. Costs +1,718 B flash / +220 B RAM measured.

The interesting part is loop prevention: two nodes both answering every chat
volley forever, so replies are marked with a new envelope flag
(`MN_F_AUTOMATED`, bit 6) that responders refuse to answer, backed by a
per-peer cooldown and a global reply budget.

See **`../docs/BOT-MODE.md`**, which also carries the effort analysis for
bot 1 (a gen-1 ELIZA) and why this Forth engine cannot host one as a script.

## Mesh time (all builds) — validated 2026-08-04

A Thread-only mesh has no border router and therefore no SNTP, so `esp_timer`
only ever gives uptime. `TIME SET <epoch> [<tz-min>]` seeds **one** node from a
host — `python tools/hcp.py synctime` does it in one command — and that node
becomes stratum 0 and multicasts the clock as a new system command (ns 0x00,
cmd 0x04 announce / 0x05 request). Everyone else adopts it at stratum+1;
`TIME SYNC` pulls on demand and gets exactly one jittered, suppression-guarded
answer whatever the mesh size. Costs +2,790 B flash / +620 B RAM.

Bench-verified: seed → full 4-node convergence in under a second, every node
within 1 s of the host, `TIME SYNC` drawing exactly one answer, and a rebooted
follower re-adopting 1.3 s after boot with no host involvement.

**Know the stratum ratchet before deploying:** the clock is RAM-only, so when
the stratum-0 node reboots the mesh loses its only real source and each
subsequent reboot adopts one hop worse (observed climbing to stratum 3 across
four reflashes) until it caps out and stops distributing time. It self-heals
while an anchor lives, and one `TIME SET` restores the whole mesh in under
0.1 s, so the fix is operational — the app should re-seed on connect. Nodes
now emit `!WARN time-no-anchor` at stratum ≥ 2 rather than degrading silently.

Details, wire format, the stratum tie-break that stops two peers re-adopting
each other forever, and the trust model are in **`../docs/MESH-TIME.md`**.

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
| `RECENT [peer-ipv6]` | — | E-G catch-up. No arg: replay **this node's** ring to the host as `!RCHAT <chan> <id> <name> <text>` (a reconnecting phone backfills its feed — bypasses mesh dedup by design). With a peer: CoAP `GET magnet/recent` from it; frames replay through the normal RX path (missed frames emit `!CHAT`, dupes drop; `# recent N frame(s)` when done) |

## Path B derivation cost (and the watchdog it used to trip)

`CHANNEL SET <passphrase>` takes Path B: PBKDF2-HMAC-SHA256, `MN_PBKDF2_ITERS`
= 100 000. Measured on the C6: **≈7.9 s**.

That used to trip the task watchdog. mbedtls runs all 100 k iterations in one
call that never returns, and on this single-core part `mn_link` therefore
starved IDLE for the whole stretch — `E (…) task_wdt: - IDLE (CPU 0)` plus a
register dump in the middle of the headline provisioning flow. The derivation
completed correctly, so it was cosmetic, but it looked like a crash.

Fixed by open-coding the one-block PBKDF2 in `magnet_crypto.c`
(`pbkdf2_yielding`) and `vTaskDelay(1)`-ing every `MN_PBKDF2_YIELD_EVERY`
(5 000) iterations — `vTaskDelay`, not `taskYIELD`, because `mn_link` outranks
IDLE and only actually blocking lets IDLE run. ~20 yields, ~200 ms added.

Since `dkLen == hLen == 32` this is a single RFC 8018 block, so the arithmetic
collapses to `T_1 = U_1 ⊕ … ⊕ U_c` — byte-identical to the call it replaced.
Verified three ways (2026-08-01): the well-known `magnet` channel still derives
**selector 82f7 / ff05::e139:9682**, a custom passphrase still derives
**selector 7770**, and an independent Python `hashlib.pbkdf2_hmac`
implementation agrees with both. `SELFTEST` still passes all three stages.

> **Open: 100 000 iterations costs ~7.9 s, not the ~1 s the header comment
> targets.** Retuning is a security *and* compatibility decision — changing
> `MN_PBKDF2_ITERS` changes the derived root, so every already-provisioned node
> lands on a different channel and the fleet silently splits. Left alone
> deliberately; decide before the first real deployment, not after.

## Recovery: `FACTORY RESET`

Provisioning used to be one-way. A node given the wrong credential tore its BLE
stack down, joined a channel nobody could reach, and needed a USB NVS wipe to
become configurable again — fine on a bench, useless in a field.

```
FACTORY RESET            → -ERR E_CONFIRM_REQUIRED …
FACTORY RESET CONFIRM    → +OK erased, rebooting
```

Erases the whole `magnet` NVS namespace **and** NimBLE's `nimble_bond`
namespace, then reboots. The node comes back virgin: default channel, no name,
empty allow-list, no script, **new device identity**, no bonds, BLE advertising
for provisioning again.

Two deliberate choices worth knowing before you change it:

- **`nvs_erase_all` on the namespace, not a list of keys.** A key-by-key list
  is a maintenance trap — add a setting later, forget to add it here, and
  "factory reset" quietly leaves state behind. That is the one bug a reset must
  never have.
- **The identity key goes too.** A reset node is one leaving your trust domain
  (resold, redeployed, handed on). Keeping its keypair would let it carry
  whatever authority some other node's allow-list still grants it. A fresh key
  is minted on the next boot exactly as on a virgin part — so **`WHOAMI` reports
  a different `id` afterwards**, and anything that had this node allow-listed
  must re-add the new key.

Privileged like any other config verb: over BLE it needs a bonded link, over
USB-CDC physical access is the trust boundary (§4.6).

> **The BLE address does *not* change.** Advertising uses `BLE_OWN_ADDR_PUBLIC`
> — the MAC — so a phone that was bonded before the reset still lists the node
> as bonded afterwards, while the node has no key. Android raises no pairing
> prompt in that state and every privileged verb answers `E_NOT_BONDED`
> indefinitely. A host must detect this and drop its own half of the bond;
> `magnet_app` does (`recoverStaleBondAndRebond`). See `../docs/BLE-PAIRING.md`.

### Validated on hardware (2026-08-01, M5NanoC6, `esp32c6_ble`)

Provisioned a node with name `wipeme`, two admin keys, an autorun script and a
non-default channel (`correct-horse-ba`, selector 7770), then reset it:

| Check | Before | After |
|-------|--------|-------|
| `WHOAMI` id | `baf892cb` | **`1fa5d82e`** — identity regenerated |
| name | `wipeme` | `-` |
| channel | `correct-horse-ba` selector=7770 | `magnet` selector=82f7 (default) |
| admin allow-list | 2 keys | empty |
| autorun script | `: hello ." hi" ;` | empty |
| BLE | torn down (provisioned) | `# ble: provisioning window open` |

Guards: bare `FACTORY RESET` → `-ERR E_CONFIRM_REQUIRED`; `FACTORY NUKE` →
`-ERR E_SYNTAX`. New identity stable across a further reboot (regenerated once,
then persisted).

### Companion build validated on hardware (2026-08-02, M5NanoC6 `probe`, `esp32c6_ble_resident`)

First soak of the resident-BLE companion variant (magnet_app SCOPE §1 Option A),
on the 3-node default-channel bench (probe + xray1 + sdk-b):

| Check | Result |
|-------|--------|
| `CHANNEL SET qr:…` with BLE up | **BLE survives** — `STATUS … ble=up` after provisioning (the provisioning-only build tears down here) |
| Reboot while provisioned | BLE advertises at boot (`# ble: advertising (resident — survives provisioning)`), Thread attaches, READY as router |
| BLE-GATT HCP while Thread up | macOS central (bleak): bonded Just-Works, encrypted, `WHOAMI`/`STATUS`/`SYSINFO` answered over GATT, `!HEARTBEAT` streams |
| Held connection | 8 min continuous, zero drops, 130 HCP lines notified |
| Mesh chat during BLE hold | multicast from probe received on xray1 mid-hold |
| Heap (advertising only) | 176.4 KB free, flat across a ~15-min / 30-cycle poll soak |
| Heap (BLE connected) | 173.9 KB free — connection costs ~2.5 KB, fully returned on disconnect |
| `STATUS` | now reports `ble=up|off` (all builds; `off` where BLE is compiled out) |

> **Stale-name gotcha:** the advertised name `MagNET-XXXX` snapshots
> `mn_device_id()` at `mn_ble_start()`. On the first boot after a factory
> reset that is the provisional id, and macOS/iOS then *cache* the GAP name
> per peripheral. Hosts must select by service UUID or address, never by the
> `MagNET-` suffix matching the device id.

**Real bug found and fixed by this pass — serial TX stall starved the BLE
path.** With no host reading USB-CDC (the normal state for a phone-only
companion node) the driver's TX ring fills and `link_putc`'s blocking write
waited its full 100 ms *per character*. Every line — BLE responses included —
funnels through the one TX writer, so each verb's dispatch queued behind the
previous response's multi-second serial stall; past 6 queued commands the BLE
RX queue dropped verbs silently. Symptom: STATUS answers, then responses
arrive one-write-late with growing latency (0.06 s → 1.4 s → 8.9 s → 11.9 s →
never) while `!HEARTBEAT` stays punctual (its BLE mirror fires before its own
serial write). Every earlier BLE test accidentally passed because a serial
poller (`hcp.py`) was draining the port at the time. Fix in `link_putc`:
20 ms grace on first full-buffer write, then drop serial output at 0 timeout
until a write succeeds again — serial can never again stall the node.
macOS repro before fix: 7/9 verbs timed out with no serial reader, 0/9 with
one attached; after fix: 0/9 timeouts, all responses 0.06 s, no reader.

**Phone hardware pass after the fix (Sharp SH-53D, Android 14): 17/17.**
`ble_probe_main.dart` pinned to the companion node: scan, connect, bond
(survives node reflash — NVS keeps bonds), STATUS/WHOAMI/CAPS/CHANNEL SHOW
round-trips, `E_UNKNOWN_VERB`, privileged `NAME` on the bonded link, and
**operator enrolment over BLE** (`ADMIN ADD` + idempotent re-add) — the
phone's operator key is now on the companion node's allow-list, which was
the SCOPE M2 check still marked untested on hardware.

Not yet done: multi-hour soak and throughput under saturation with a BLE
client attached (§7 numbers were taken without BLE).

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

## E-G: scale + SED catch-up + hardening (fw 0.6.0-eg, validated 2026-08-03)

The last firmware phase of the design proposal (§11.5/§11.6). Bench: **4 nodes**
— `probe` (resident companion), `xray1`, `sdk-b`, `xray2` (all M5NanoC6-class,
default `magnet` channel).

**What shipped:**

- **SED / late-joiner catch-up.** Every accepted multicast chat frame (sent or
  received, raw wire bytes — still ciphertext) lands in a 12-slot ring;
  `GET magnet/recent` (CoAP) serves it as `[2B BE len][frame]…` oldest-first in
  one response (≤960 B, no block-wise). New HCP verb **`RECENT <peer-ipv6>`**
  fetches and replays the frames through the normal RX path, so decrypt +
  high-water dedup give catch-up semantics for free: you get exactly what you
  missed, dupes drop silently. DMs are never stored — a poll cannot leak
  someone else's unicast. The ring clears on `CHANNEL SET`. A second, no-arg
  form — **`RECENT`** — replays *this node's own* ring to the host as
  `!RCHAT <chan> <id> <name> <text>` lines (deliberately bypassing mesh dedup,
  which would swallow known frames): it's how the phone app backfills its feed
  on reconnecting to the companion.
- **§11.6 scale sizing.** Peer + replay tables 16 → 40 slots (an undersized LRU
  re-admits replayed counters and re-fires `!PEER_JOIN` at 32+ nodes), `MESH`
  neighbor list 8 → 16, event pump queue 8 → 12 (a full catch-up response
  replays 12 frames back-to-back). Net RAM cost ≈ 13 KB (measured).
- **Jittered announce (Trickle-style suppression).** On a partition heal every
  node hits READY in the same MLE beat; announcing immediately is a
  synchronized burst that scales with node count. The READY announce now waits
  a random 200–1700 ms (one-shot timer; rapid role flaps collapse into one
  announce). Forwarding-layer suppression is already Thread's job: multicast
  rides **MPL, which *is* Trickle** (RFC 7731) — the app layer adds the
  sender/counter dedup and this jitter, not another rebroadcast layer.
- **ESP32forth port decision: the stub stays.** The spec gated the full
  v7.0.8.0 port on "iff the stub blocks real scripts". Through E-E/E-F it never
  did — hooks, autorun scripts and the FFI vocabulary all run on the stub. The
  port (and the `( xt -- )` hook form) stays deferred until a script actually
  needs it.

**Validation scorecard (all on hardware):**

| Test | Result |
|------|--------|
| Selftest, all 4 nodes | 4/4 PASS (env roundtrip, event pump, CoAP loopback) |
| Catch-up, hard outage | sdk-b held in **bootloader** through 3 chats → boots with `rx msgs=0` → `RECENT <xray1>` replays 6 frames as `!CHAT` |
| Catch-up idempotence | second fetch: 6 frames served, **6 deduped, 0 emitted** |
| Catch-up vs MPL window | a *fast* reboot needs no catch-up: MPL's Trickle retransmissions delivered the "missed" multicast on reattach, and the later `RECENT` replay deduped cleanly (`msgs=3 dup=3`) |
| Leader failover | leader (xray2) radio-dead via bootloader hold → xray1 forms new partition + takes leadership, observed **≤ ~15 s**; sdk-b + probe merge; xray2 rejoins as child — no partition fight |
| Saturation (STRESS 15 s, 200 B, all 4 nodes BLE up) | sender accepted 30.1 msg/s (OT backpressure rejects the rest, as designed); per-receiver delivery 74–93/452 (16–21%), `rx err=0`, MPL dupes silently suppressed |
| Announce jitter | names propagate to all peer tables after every reboot/rejoin in the run |
| Heap, resident build (worst case) | 161.7 KB free / 160.5 KB min-ever — E-F baseline minus the ~13 KB of new tables, no leak |

Saturation context: 200 B ⇒ 2–3 802.15.4 fragments, unacknowledged multicast —
same congestive-collapse regime the E-B table above characterizes, now with
NimBLE advertising on all four nodes sharing the 2.4 GHz front end. Chat-rate
traffic remains the E-B story (lossless single-frame at 1 msg/s/node).

> **Bench technique — killing a node for real:** RTS "hold in reset" power-
> cycles the C6 (the USB bridge is on-die; see the E-B note above). What *does*
> work without touching cables: `esptool.py --after no_reset chip_id` leaves
> the chip parked in the ROM bootloader — radio genuinely dead until the next
> RTS pulse (`hcp.py <port> --reboot`). That is how the catch-up outage and the
> leader kill above were staged.

**Multi-hour soak (2026-08-03, 4 nodes, fw 0.6.0-eg) — PASS.** `tools/soak.py`
held all four serial ports open for **5 h 40 m** (the run was cut ~20 min short
of its 6 h target by a host-side USB re-enumeration that took out every port at
once — a Mac/hub event, not firmware; all four nodes stayed up through it, per
their uptime counters). Round-robin `CHAT` every 60 s (~340 chats), `STATS` +
`SYSINFO` sweeps every 10 min:

| Metric | Result |
|--------|--------|
| Delivery (each chat → other 3 nodes) | **~1,017/1,017 — zero misses over the whole run** |
| Heap drift, first → last sweep (330 min) | probe/sdk-b/xray2 **byte-identical**; xray1 −132 B (fragmentation noise; its min-ever never moved) |
| min-ever heap movement | ≤ 4 B on one node, 0 on the rest — no slide |
| `rx err` / `rx dup` | 0 / 0 on all four nodes at every sweep |
| Port drops, `-ERR` lines | none until the terminal host-side USB event |

Chat-rate traffic is lossless over multi-hour spans, and the E-G tables
(40-entry peer/dedup) leak nothing. This soak now runs unattended: the
`Jenkinsfile` at the design root gates on `soak.py --gate` (delivery ≥ 99.5 %,
per-node heap loss ≤ 8 KB, `rx err=0`).

**32+ node soak — status and plan.** Four nodes is the bench ceiling; the 32+
soak needs hardware that doesn't exist here yet. What E-G changes ship ready
for it: tables sized for 40 senders, heal-storm jitter, catch-up for nodes that
sleep through traffic. The soak plan, when hardware lands: (1) 32× C6 on one
powered USB fabric, staggered boot; (2) steady-state 1 msg/s/node paced chat
for 24 h — expect 0% single-frame loss per the E-B steady-state table, watch
`rx dup` (MPL suppression working) and `!PEER_JOIN` churn (table thrash — must
be zero); (3) leader kill + partition-heal cycles hourly via the bootloader
trick, watching announce-burst collapse; (4) heap min-ever on every node
before/after. Go/no-go: no WDT, no heap slide, dedup tables never evict a
live sender.

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
