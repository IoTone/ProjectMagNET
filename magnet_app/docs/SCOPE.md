# MagNET app — scope

**What this is:** the configuration, discovery and test console for MagNET
Hanasu mesh nodes. The thing you hold while standing next to hardware.

**Status:** 2026-07-31. Written after E-Phase F; firmware is at `0.5.0-ee`
with a 4-node bench (2× M5NanoC6 + 2× XIAO ESP32C6).

---

## 1. The constraint that shapes everything

A Hanasu node advertises its BLE HCP service **only while unprovisioned**. The
moment it takes a channel credential it tears the BLE stack down and hands the
2.4 GHz front end to Thread — that was a deliberate call (§12.9 Q3, settled
2026-07-31) and it is the right one for deployed hardware.

The consequence for this app: **a working mesh is invisible to it.** BLE alone
buys us exactly one interaction — configure a node once, then lose it.

That is fine for a provisioning tool. It is fatal for a *test and discovery*
tool, which is what this document is scoping. So the first question is not
"what screens do we build" but "how does the app reach a node that is already
doing its job".

### Options for reaching operational nodes

| # | Path | Reach | Cost | Verdict |
|---|------|-------|------|---------|
| A | **Companion node over BLE** — one node runs a `esp32c6_ble_resident` build that keeps BLE up, and acts as the app's window into the mesh (§4.7 Option B, the "node as radio modem" model the spec already assumes) | Everything the mesh carries: chat, peers, events | ~1 day firmware (build flag + skip the teardown), coexistence soak | **Recommended.** Matches the spec's own host model, keeps deployed nodes locked down, and one bench node becomes the diagnostic anchor. |
| B | **USB serial from a desktop build** | Full HCP, all verbs | Flutter desktop + a serial plugin (`flutter_libserialport`); macOS/Windows only, no phone | Useful *later* as a lab tool; duplicates the Python SDK, which already does this well. |
| C | **WiFi/WebSocket via a border router** | Whole mesh, remotely | Significant firmware (§11.2 WebSocket binding + WiFi coexistence on C6) | Right long-term answer; out of scope until E-G. |
| D | Resident BLE on *every* node | Everything, everywhere | Reopens the coexistence and bonding-window risks we closed | Rejected. |

**Recommendation: A now, C later, B only if a lab need appears.** Option A costs
almost nothing in firmware (the teardown call is already conditional) and turns
the phone into the §11.2 host the protocol was designed around.

> **Decided 2026-08-02: Option A** (C later, B only if a lab need appears).
> Firmware env `esp32c6_ble_resident` landed in `firmware-idf/platformio.ini` —
> same source as `esp32c6_ble` with `-DMN_BLE_RESIDENT=1`, which skips the
> post-provisioning teardown and advertises regardless of provisioning state.
> Per-verb bonding enforcement (E_NOT_BONDED) is unchanged. **First coexistence
> soak passed 2026-08-02** on the bench node `probe` (M5NanoC6): BLE survives
> `CHANNEL SET` and provisioned reboot, 8-min held GATT connection with HCP
> streaming while mesh chat flows, heap flat (~176 KB advertising / ~174 KB
> connected). Scorecard in `firmware-idf/README.md`. `STATUS` now reports
> `ble=up|off` for the companion-node picker. Milestone 3 is unblocked.

---

## 2. Milestones

### M1 — Provisioning, finished and proven *(hardware pass DONE 2026-07-31, 11/11)*

The one flow that works over provisioning-only BLE.

**The hardware pass found three real bugs that unit tests could not.** Run it
again after any BLE change:

```bash
flutter run -d macos -t lib/tool/ble_probe_main.dart
```

| Found | Fix |
|-------|-----|
| Service UUID wrong on air. `BLE_UUID128_INIT` takes **exactly 16 bytes**; an earlier "fix" passed 18, and the trailing `ma` of `magn` was silently dropped — the build never complained | Corrected to 16 little-endian bytes; probe now reports `6d61676e-2d68-6370-0001-000000000000` |
| Nothing ever initiated pairing, so `WRITE_ENC` writes failed with an opaque `Encryption is insufficient` | Node calls `ble_gap_security_initiate()` on connect, and handles `ENC_CHANGE` / `REPEAT_PAIRING` |
| ATT-layer encryption gating blocked **read-only** verbs too, and surfaced as an ATT error the host can't act on | Bonding is enforced **per verb** instead: read-only verbs stay open, privileged ones return `-ERR E_NOT_BONDED` — which is what §11.3.4 defined that code for |

**Android pass (Sharp SH-53D, Android 14): 13/13.** Run it with

```bash
flutter run -d <device-id> -t lib/tool/ble_probe_main.dart
```

Four more findings, all Android-only, none of which unit tests or the macOS
run could have surfaced:

| Found | Fix |
|-------|-----|
| App crashed instantly: `ClassNotFoundException: io.iotone.magnet.MainActivity`. Renaming `applicationId` left the Kotlin class in the old package, and Android resolves `.MainActivity` against the application id | Moved `MainActivity.kt` to `io/iotone/magnet/`; aligned the stale iOS bundle ids too |
| Probe printed nothing on device. `dart:io` `stdout` reaches a desktop console but **is not wired to logcat** | All probe output goes through `print()`, tagged `[probe]`, and now also renders on-screen — a probe in your hand shouldn't be a blank rectangle |
| Scans returned **zero devices** with permissions granted. System log: `Permission denial: Need ACCESS_FINE_LOCATION permission to get scan results` — Android 12+ withholds results unless *precise* location is granted, and `locationWhenInUse` may only be *approximate* | Declared `android:usesPermissionFlags="neverForLocation"` on `BLUETOOTH_SCAN`. This app finds nodes, it does not infer location — so no location permission is needed for BLE at all now |
| Still zero devices, intermittently. System log: `Cannot start unfiltered scan in screen-off` | Environmental: the screen must be on for an *unfiltered* scan. See the advertising note below — this is a product limitation, not just a test one |
| `CAPS` write timed out at 15 s *even though the full response arrived*, then the link dropped and every later write failed | The firmware dispatched the whole verb **inside the ATT write callback**, so NimBLE could not acknowledge the write until the node had pushed every notification chunk. Commands now go to a worker task — the same event-pump discipline the OpenThread path has used since E-B |

Also confirmed: macOS/iOS/Android all withhold service UUIDs from
advertisements (`services=[]`), so the name-prefix fallback is load-bearing.

> ~~Product gap worth fixing~~ **FIXED 2026-08-02:** the advert now carries
> the 128-bit HCP service UUID (hosts can run *filtered* scans — works
> screen-off on Android and in the background on iOS) and the `MagNET-XXXX`
> name moved to the scan response. Verified on air on both bench builds
> (`esp32c6_ble` on xray1, `esp32c6_ble_resident` on probe). Bonus: active
> scans now refresh the platform's cached GAP name, so the stale-name
> gotcha self-heals for scanning hosts.

- [x] BLE scan, node detection by name prefix + service UUID
- [x] Connect, discover service, subscribe to notifications
- [x] Read `STATUS` / `WHOAMI` / `CHANNEL SHOW`; set `NAME`; `CHANNEL SET`
- [x] HCP client with `@tag` correlation, typed verbs, `E_*` errors (9 unit tests)
- [x] **Run it against real hardware** — scan, connect, discover, notify,
      round-trip, chunk reassembly, error codes, and bonding gate all verified
- [x] Credential helpers *(shipped 2026-08-02)*: Generate phrase (13 EFF
      short-list words ≈ 134 bits, Path C) / Generate secret (`qr:` 256-bit,
      Path A) buttons, live path+entropy hint under the field, QR display
      (in the provisioned dialog and on demand) for enrolling the next
      device, and a typed-passphrase (Path B) warning in the confirm dialog
- [x] Remember what we provisioned *(shipped 2026-08-02)*: `ProvisionLog`
      records id, name, channel *label* (never the credential — it's a key),
      selector, path, timestamp; viewer at Settings → Provisioned nodes

*Ends when:* a factory-fresh node goes from box to meshed without a cable.

### M2 — Operator identity *(built 2026-07-31; enrolment hardware-validated 2026-08-02 — `ADMIN ADD` + idempotent re-add over bonded BLE, probe run 17/17 on the companion node; signed fleet commands still untested)*

The phone is the fleet's admin key holder. The firmware already enforces the
allow-list, so this needed no new C.

- [x] Operator ECDSA P-256 keypair in the platform keystore
      (`lib/magnet/operator_identity.dart`, 5 unit tests)
- [x] `ADMIN ADD` during provisioning — **before** `CHANNEL SET`, because that
      verb tears the BLE stack down and nothing can be done over the link after it
- [x] `ADMIN LIST` review, with a warning card when the allow-list is empty
- [ ] Signed `ROTATE` from the app (needs a live node link — gated on M3)
- [x] **Enrolment on hardware (2026-08-02, 17/17)** — bonded BLE link to
      `MagNET-98db`, `ADMIN ADD` accepted, key `04e3feeb3e560aeb…` verified
      present in the node's `ADMIN LIST` over serial, and re-adding proven
      idempotent (the allow-list is only 4 slots and provisioning gets retried)

> **`cryptography` is unusable here.** Its P-256 is a platform-binding shim
> whose pure-Dart path throws `UnimplementedError` — it cannot be unit-tested
> and fails wherever the native binding is absent. Swapped for `pointycastle`,
> which does keygen, SEC1 encoding and signing in pure Dart everywhere.

> **Bonding works as of 2026-08-01** — `enc_change status=0`, `NAME` accepted
> over BLE, real bond recorded on the phone (14/14 on the Android probe).
>
> The hard-won lesson: **only the OS pairing prompt can complete a bond.** An
> in-app "Pair now" button caused eight consecutive false failures, because
> tapping it left the real system request unanswered. There is now no in-app
> pairing affordance anywhere; the app follows `device.bondState` and blocks
> privileged actions behind a non-dismissible dialog until the platform
> reports bonded, declined, or timed out.
>
> See **`reference-designs/MagNET_Thread_COaP_hanasu_esp32c6/docs/BLE-PAIRING.md`**
> for the full account of what is established on both the NimBLE and Android
> sides, and — importantly — what remains unverified (bond persistence across
> reboot, re-pairing after NVS erase, `CHANNEL SET` over BLE end to end, iOS
> entirely, and whether `ble_store_config_init()` was actually required).

*Ends when:* a node provisioned by this phone will accept a fleet command from
it and refuse one from anything else.

### M3 — Live mesh view *(shipped + hardware-verified 2026-08-02 on SH-53D against the `probe` companion)*

With a companion node the app stops being a one-shot configurator.
Implementation: `lib/magnet/mesh_session.dart` (session/state) +
`lib/screens/mesh_screen.dart` (picker + Feed/Peers/Topology tabs), entry
via the Dashboard "Live mesh" card, route `/mesh`.

- [x] Companion-node picker: choose which node is the window, remember it
      (remembered by **device id** — the advertised name is cacheable and
      can lie, see firmware README's stale-name note)
- [x] `SUB` to event classes; render `!CHAT`, `!PEER_JOIN/LEAVE`, `!ROLE`,
      `!STATE`, `!WARN` as a live feed (heartbeats update status silently)
- [x] Peer table from `PEERS`, with names, last-seen — refreshed live when
      a peer chats or joins
- [x] Chat send/receive — the app becomes a mesh client, not just a tool
- [x] Mesh topology from `MESH`: role, partition, RLOC16, neighbours + RSSI
- [x] **Feed backfill on connect** *(added 2026-08-03, needs fw ≥ 0.6.0-eg)*:
      the app sends no-arg `RECENT` after subscribing; the companion replays
      its ring as `!RCHAT` events → rendered as dimmed "earlier" chat items,
      deduped against anything the feed already shows. Older firmware answers
      `E_UNKNOWN_VERB` and the feed simply stays live-only.
- [x] **Clock seed on connect** *(added + hardware-verified 2026-08-04 on
      SH-53D against `probe`; needs fw with `TIME`)*:
      the app sends `TIME SET <epoch> <tz-minutes>` on every connect. A
      Thread-only mesh has no border router and therefore no NTP, so the phone
      is the only participant with a real clock; the companion takes the seed
      as stratum 0 and multicasts it to the whole channel. Doing it on every
      connect is deliberate — node clocks are RAM-only, so a rebooted node
      adopts from a neighbour one hop further from a real source, and left
      alone that ratchets until the mesh stops distributing time (see the
      reference design's `docs/MESH-TIME.md`). Older firmware answers
      `E_UNKNOWN_VERB` and nodes just report uptime. Covered by
      `test/magnet/mesh_session_test.dart`. Verified on the bench: `probe` went
      from `stratum=1 src=2ca44570` to `stratum=0 src=host` on connect, twice,
      and the mesh then re-anchored on it. Note the seed rides on *successful*
      connects — after a dropped link the screen stays in `Connection lost`
      until reconnect is tapped, so it is not automatic from the error state.

*Ends when:* you can watch mesh traffic and talk to the mesh from the phone.
**Done** — bench chat from `xray1` rendered live on the phone through
`probe`, chat sent back from the composer.

> Fixed along the way: `HcpClient.lastComments` raced when two
> comment-parsing commands were queued concurrently (the next command's
> clear could beat the previous caller's read). `commandCaptured()` snapshots
> the comments atomically inside the command queue; PEERS/MESH use it.

### M4 — Field test console *(built 2026-08-02 as the Mesh screen's Test tab; on-phone verification pending)*

Everything the bench scripts do this session, but in your hand. Lives on
the companion connection (`mesh_test_tab.dart`) — the only node reachable
once the fleet is provisioned.

- [x] Run `SELFTEST` and show the three stages pass/fail (parsed from the
      `# selftest <stage> ok|FAIL` lines, plus the overall verdict)
- [x] `STATS` with deltas over time — each Sample keeps the previous
      snapshot and renders Δ/s per counter
- [x] `BENCH` (envelope codec + TX latency) surfaced as a one-tap check
- [x] `STRESS <secs> <len>` with a live progress feed (mirrors `# stress`
      commentary) — guarded behind a confirm, since it saturates the channel;
      privileged, so it also exercises the bonded link
- [x] Export a node report (JSON + shareable text, copied to clipboard):
      identity, status, channel, sysinfo, counters, selftest, timestamp
- [x] Diagnostics for the boring failures: adapter off / permission denied
      (from the BLE adapter state), out of range / node busy / E_NOT_BONDED
      mapped to plain-language messages

*Ends when:* the bench Python scripts have no capability the phone lacks.

---

## 3. Explicitly out of scope

- **Hive AI devices** (M5 Dial / Atom Echo / Camera / Capsule). They speak HTTP
  over WiFi with mDNS discovery — a different protocol, different transport,
  different data model. Sharing the device list is possible later; it is not
  this project.
- **Firmware OTA.** The 4 MB parts have no dual-slot table (see
  `firmware-idf/README.md`); this is a hardware/BOM decision first.
- **Remote/cloud access.** Everything here is proximate — BLE range or the
  companion node's mesh. Border-router routing is E-G firmware work.
- **Android-specific BLE quirks** beyond what `flutter_blue_plus` handles.
  Test on iOS first, then widen.

---

## 4. Risks

| Risk | Why it bites | Mitigation |
|------|--------------|------------|
| **Nothing in the BLE path has run on hardware** | Written from the spec and the firmware source, verified only by unit tests and a LightBlue poke. Bonded writes are the most likely to surprise. | M1's hardware pass, before anything is built on top |
| ~~Provisioning is one-way~~ **closed 2026-08-01** | A mis-provisioned node needed an NVS wipe over USB to become configurable again | `FACTORY RESET CONFIRM` shipped and hardware-validated — erases channel, name, allow-list, script, identity and BLE bonds, reboots advertising. Note it mints a **new device id**, so the app must re-read `WHOAMI` and re-enrol rather than assume the id it knew |
| iOS hides service UUIDs pre-connect | Filtering by service alone would show nothing | Already handled: name-prefix **or** service match |
| Companion-node build drifts from the deployed build | Two firmware variants diverge quietly | Same source, one build flag; CI builds both |
| Seed-phrase UX invites weak input | A typed phrase is Path B (PBKDF2, weaker) not Path C | Generate by default, warn on typed, show the derivation path in the UI (already displayed) |

---

## 5. Suggested order

1. **M1 hardware pass** — an afternoon, and it either validates or invalidates
   everything below it.
2. **M1 credential helpers** — small, and it fixes the weakest part of the
   security story (people typing passphrases).
3. ~~Decide the §1 question.~~ **Decided: Option A** — the
   `esp32c6_ble_resident` env exists; flash one bench node with it and soak.
4. **M3**, because a live mesh view is what makes this a tool you reach for
   rather than a wizard you run once.
5. **M2** and **M4** in either order.

M1 + M2 need no firmware work at all. M3 and M4 need a companion node flashed
with `esp32c6_ble_resident` on the bench.
