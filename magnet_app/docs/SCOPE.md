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

> **Decision needed** before Milestone 3 can start. Milestones 1–2 do not
> depend on it.

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

> **Product gap worth fixing (not test-only):** the node advertises its name
> but not its service UUID, so a host can only find it with an *unfiltered*
> scan — which Android forbids while the screen is off, and which iOS
> deprioritises in the background. The 31-byte advert cannot hold both a
> 128-bit UUID and the name; moving the name into the **scan response** and
> putting the UUID in the advert would fix discovery in both cases.

- [x] BLE scan, node detection by name prefix + service UUID
- [x] Connect, discover service, subscribe to notifications
- [x] Read `STATUS` / `WHOAMI` / `CHANNEL SHOW`; set `NAME`; `CHANNEL SET`
- [x] HCP client with `@tag` correlation, typed verbs, `E_*` errors (9 unit tests)
- [x] **Run it against real hardware** — scan, connect, discover, notify,
      round-trip, chunk reassembly, error codes, and bonding gate all verified
- [ ] Credential helpers: generate a 12–24 word seed phrase or a 256-bit
      `qr:` secret in-app, show it as a QR for the next device (§11.4 is
      explicit that typing a passphrase is the *fallback*, not the path)
- [ ] Remember what we provisioned: local record of id, name, channel label,
      when — so the fleet is knowable even after nodes go dark

*Ends when:* a factory-fresh node goes from box to meshed without a cable.

### M2 — Operator identity *(built 2026-07-31; enrolment untested on hardware)*

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

### M3 — Live mesh view *(depends on the §1 decision)*

With a companion node the app stops being a one-shot configurator.

- [ ] Companion-node picker: choose which node is the window, remember it
- [ ] `SUB` to event classes; render `!CHAT`, `!PEER_JOIN/LEAVE`, `!ROLE`,
      `!STATE`, `!WARN` as a live feed
- [ ] Peer table from `PEERS`, with names, last-seen, and mesh role
- [ ] Chat send/receive — the app becomes a mesh client, not just a tool
- [ ] Mesh topology from `MESH`: role, partition, RLOC16, neighbours + RSSI

*Ends when:* you can watch mesh traffic and talk to the mesh from the phone.

### M4 — Field test console *(the "test" half of the brief)*

Everything the bench scripts do this session, but in your hand.

- [ ] Run `SELFTEST` and show the three stages pass/fail
- [ ] `STATS` with deltas over time, not just totals — loss and rate are the
      numbers that matter, and they only exist as differences
- [ ] `BENCH` (envelope codec + TX latency) surfaced as a one-tap check
- [ ] `STRESS <secs> <len>` with a live progress feed and a result card —
      guarded behind a confirm, since it saturates the channel
- [ ] Export a node report (JSON + shareable text): identity, firmware,
      channel, counters, selftest, timestamp
- [ ] Diagnostics for the boring failures: adapter off, permission denied,
      out of range, node busy

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
3. **Decide the §1 question.** If Option A, the firmware change is one flag.
4. **M3**, because a live mesh view is what makes this a tool you reach for
   rather than a wizard you run once.
5. **M2** and **M4** in either order.

M1 + M2 need no firmware work at all. M3 and M4 need the companion-node
decision first.
