# BLE pairing & bonding — what we know, and what we don't

Bonding took far longer to get working than it should have, and most of the
cost was chasing the wrong layer. This records what is now established (with
the evidence for it), and what remains genuinely unverified, so nobody repeats
the same loop.

Covers both sides: the node (**NimBLE / ESP-IDF 5.3.1**) and the host app
(**Flutter / flutter_blue_plus**, tested on a Sharp SH-53D, Android 14).

**Status: bonding and operator enrolment both work.** 2026-08-01 —
`enc_change status=0`, `NAME` accepted over BLE, real bond on the phone.
2026-08-02 — `ADMIN ADD` accepted over the bonded link and verified in the
node's allow-list over serial (probe: 17/17).

> **Two build flavours since 2026-08-02.** The default `esp32c6_ble` build
> advertises **only while unprovisioned** and tears BLE down after
> `CHANNEL SET` (a power cycle must not re-open a bonding window on a deployed
> node). The `esp32c6_ble_resident` build (`-DMN_BLE_RESIDENT=1`) skips both —
> it stays attachable forever and is what a *companion* node runs so a phone
> has a standing window into the mesh. Everything below applies to both.

---

## 1. The one rule that explains most of the pain

> **Only the operating system's own pairing prompt can complete a bond.
> Nothing in the app can stand in for it.**

An in-app "Pair now" button is worse than no button: a user taps it believing
they have answered, the real system request goes unanswered, and the link dies
~30 s later with a generic timeout. That is precisely what happened here, and
it produced eight consecutive "failures" that were all the same missed prompt.

The app's job is to **announce, wait, and report** — never to offer a
substitute action. `MagnetBleTransport.bond()` and `_ensureBonded()` are built
around that, and both carry comments saying so.

---

## 2. Established — node side (NimBLE)

| Finding | Evidence |
|---|---|
| **A peripheral's Security Request alone is ignored.** `ble_gap_security_initiate()` on connect produced no `ENC_CHANGE` at all. | Node console showed connect, no encryption event ever. |
| **A central pairs when an *operation* requires encryption.** Adding an encryption-required characteristic (HCP-AUTH, `…0003`, `READ_ENC`) made Android begin pairing immediately on connect. | `ENC_CHANGE` started firing once the characteristic existed. |
| **`enc_change status=13` is `BLE_HS_ETIMEOUT`** — the SMP exchange started and nobody answered. It is *not* a crypto or config failure. | Consistent across every unanswered attempt; `status=0` the moment the prompt was accepted. |
| **Never do work in the ATT write callback.** NimBLE cannot acknowledge the write until the callback returns, so dispatching a verb inline made the host's write time out *even though the node had answered correctly*. | `CAPS` timed out at 15 s with the full response visible in the traffic log. Fixed with a queue + worker task. |
| **`BLE_UUID128_INIT` takes exactly 16 bytes.** Passing 18 silently truncates and leaves the wrong UUID on the air, with no build warning. | Service undiscoverable until corrected; probe then reported the right UUID. |
| **Bonding is enforced per verb, not at the ATT layer.** Gating the shared command characteristic with `WRITE_ENC` also blocked read-only verbs and surfaced as an opaque ATT error. | Read-only verbs now work unbonded; privileged ones return `-ERR E_NOT_BONDED` (§11.3.4). |
| **Notification chunks must be retried.** NimBLE's mbuf pool is small; a burst (a long `CAPS` line) exhausts it and a dropped chunk means the host never sees a terminating newline. | Fixed with bounded retry + yield. |
| **A stalled serial writer starves BLE.** With no USB-CDC reader attached, `link_putc` blocked 100 ms/char and every BLE response queued behind it: responses arrived one-write-late with growing latency, then the 6-deep BLE RX queue dropped verbs outright. Earlier BLE tests passed *only because* `hcp.py` happened to be draining serial. Fixed with a 20 ms grace then drop-at-0-timeout until a serial write succeeds. | Resident-companion soak 2026-08-02: heartbeats stayed punctual (their BLE mirror fires first) while command responses lagged exactly one exchange behind — the signature to recognise. |
| **Platforms cache the GAP name.** `MagNET-xxxx` snapshots the device id at `ble_start`, and macOS/iOS keep showing a stale cached name long after it changes. Hosts must select by **service UUID or address**, never by name suffix. | Observed on macOS + iPhone 2026-08-02; the app and probes now match on UUID/address. |
| **A long (queued) write is a *complete* message, not a fragment.** The inbound boundary rule was `complete = len < mtu-3`, so any write longer than one ATT payload — which NimBLE has already reassembled from a prepare/execute sequence — was parked in the line buffer forever. No response, no error, nothing on the wire. Short verbs worked, which is what made it so confusing. Correct rule: only a write *exactly* equal to `mtu-3` is ambiguous, so `complete = (len != mtu-3)`. | `ADMIN ADD <130-hex pubkey>` (≈148 bytes with its `@tag`) timed out over BLE while `NAME`/`STATUS`/`CAPS` all worked, and the node's allow-list stayed empty (2026-08-02). |

### Security settings (match a known-good NimBLE-Arduino config)

```c
ble_hs_cfg.sm_bonding  = 1;
ble_hs_cfg.sm_mitm     = 0;                          // no display, no keypad
ble_hs_cfg.sm_sc       = 1;                          // LE Secure Connections
ble_hs_cfg.sm_io_cap   = BLE_HS_IO_NO_INPUT_OUTPUT;  // ⇒ Just Works
ble_hs_cfg.sm_our_key_dist = sm_their_key_dist =
    BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
```

Equivalent to NimBLE-Arduino's `setSecurityAuth(true, false, true)` +
`setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT)`, which is known to work in
another project on this fleet. **The parameters were never the problem** —
confirming that is what stopped us tuning them.

`CONFIG_BT_NIMBLE_SM_LEGACY=y` and `SM_SC=y` are both enabled so a peer that
will not complete Secure Connections has a fallback.

---

## 3. Established — app side (Flutter / Android)

| Finding | Evidence |
|---|---|
| **Android posts the pairing request as a *notification*, not a dialog** ("Tap to pair with MagNET-2a19"). Nothing takes window focus, so an unattended run just watches it time out. | `dumpsys notification` showed it; `mCurrentFocus` never changed. |
| **Tapping the collapsed notification row is not the action.** The "Pair & connect" button is not rendered while collapsed, and tapping the row did nothing. | Tapped via adb at the row's bounds; no bond formed. |
| **`createBond()` alone does not finish it** — it starts the same exchange that still needs the prompt answered. | Same `status=13`. |
| **Unfiltered BLE scans are refused while the screen is off.** | `W/BtGatt.ScanManager: Cannot start unfiltered scan in screen-off`. |
| **Scan results are withheld without *precise* location**, unless `BLUETOOTH_SCAN` declares `neverForLocation`. Permission granted ≠ results delivered; the scan silently returns zero devices. | `E/BluetoothUtils: Permission denial: Need ACCESS_FINE_LOCATION permission to get scan results`. |
| **A timed-out bond attempt kills the connection**, so anything attempted afterwards fails with `device is not connected`. Bond early, on a fresh link. | Reordering to bond-at-connect is what finally worked. |
| **`dart:io` `stdout` is not wired to logcat.** Use `print()`. | Probe output invisible on device until changed. |
| **A stale bond is invisible to the platform and never self-heals.** After `FACTORY RESET` the node keeps its BLE address (it advertises `BLE_OWN_ADDR_PUBLIC` = the MAC) but loses its keys. Android still lists it as bonded, so it offers an LTK the node cannot match, raises **no** prompt, and `bond()` short-circuits to `alreadyBonded`. Every privileged verb then returns `E_NOT_BONDED` forever. | Observed 2026-08-02: node logged `# ble: link NOT encrypted (enc_change status=13)`, then `-ERR E_NOT_BONDED`, with no pairing request shown on the phone. |

| **Renaming `applicationId` requires moving the Kotlin package too** — `.MainActivity` resolves against the application id. | `ClassNotFoundException` crash on launch. |

> **Consequence worth stating plainly:** without explicit recovery, the act of
> factory-resetting a node makes it unprovisionable *by the very phone that
> reset it* — and `FACTORY RESET` is the documented recovery path. The app
> therefore treats `E_NOT_BONDED` on a platform-bonded link as proof of a stale
> bond and repairs it: `removeBond()` → reconnect → bond again
> (`MagnetBleTransport.recoverStaleBond*`). On iOS an app cannot forget a
> pairing at all, so there the user must do it in Settings — untested.

### The shape the app must have

- Follow `device.bondState` (`none → bonding → bonded`), not a characteristic
  read, as the source of truth.
- Block privileged actions behind a non-dismissible progress dialog whose only
  affordance is **Cancel**.
- Distinguish outcomes: `bonded`, `alreadyBonded`, `declined`, `timedOut`,
  `unsupported`. "Declined" and "ignored" need different messages.
- Retry a missed prompt rather than failing outright.
- Hold a wakelock for the exchange — a dimming screen can tear the prompt down.

---

## 4. Unknown / unverified

Listed roughly by how likely they are to bite.

| Open question | Why it matters | How to settle it |
|---|---|---|
| **Did the long-write boundary fix actually fix `ADMIN ADD`?** The rule was genuinely wrong and is now right, but the run that proved `ADMIN ADD` also logged `# ble: mtu negotiated 256` — payload 253, command ~148 bytes, which the *old* rule would have passed too. The reflash rebooted the node as well. So the change is correct on its merits, but its causal role is **unproven**. | If something else was the real cause it is still there, and will resurface on a longer command. | Re-flash the pre-fix boundary rule and re-run the probe against a bonded node. One clean variable. |
| **Was `ble_store_config_init()` actually required?** It was added at the same time as the bond-at-connect reordering, so the two are confounded. It is correct regardless (ESP-IDF examples call it; NimBLE-Arduino calls it internally), but its necessity here is **unproven**. | If it is required, omitting it in a future refactor silently breaks bonding again. | Revert the single line, rerun with correct timing. One clean variable. |
| **Does the bond survive a node reboot?** NVS persistence is enabled but never tested across a power cycle. | A bond that evaporates on reboot makes the whole model useless in the field. | Reboot the node, reconnect, check a privileged verb without re-pairing. |
| **Does re-pairing work after `FACTORY RESET`?** The `REPEAT_PAIRING` handler deletes our copy and retries, but has never executed. `FACTORY RESET` now erases NimBLE's `nimble_bond` namespace — but that erase has only been exercised on a node whose BLE was already torn down, so **the bond-erase path itself is code-verified, not observed**. | A factory-reset node that no phone can re-pair with is bricked for provisioning — and this is now the *documented* recovery path, so it has to work. | Bond a phone, `FACTORY RESET CONFIRM`, then reconnect from that same phone without forgetting the device on it. Note the node comes back with a **new device id**, so it advertises under a different `MagNET-xxxx` name — and because platforms cache GAP names (see §2), the phone may keep *displaying* the old one; match on address. |
| **Does `CHANNEL SET` over BLE behave sanely?** It tears the BLE stack down mid-command — the app's view of that is untested. Does it see `+OK`, or a disconnect first? | This is *the* headline flow; a confusing failure here undermines the product. | Provision a spare node over BLE end to end. |
| **iOS: completely untested.** No pairing, no discovery, nothing. The auth-characteristic trigger exists precisely because iOS has no `createBond()`, but that path has never run. | Half the target platforms. | Run the probe on the iPhone. |
| **Which pairing method did Android actually choose?** No `PASSKEY_ACTION` was ever observed, so presumably Just Works — but that is inference, not observation. | Determines whether the prompt can ever demand a code, which the node cannot supply. | NimBLE host logging at DEBUG, or read the prompt text carefully. |
| **Encrypted-link MTU/chunking.** All notification-reassembly testing was on an unencrypted link; encryption reduces usable payload per packet. | Long lines (`CAPS`) could fragment differently once bonded. | Re-run the `CAPS` check while bonded. |
| **Bond behaviour with multiple phones / multiple nodes.** One-to-one only so far. | Fleet provisioning by more than one operator. | Bond a second phone; confirm the first still works. |
| **Prompt presentation is OEM-dependent.** Notification-vs-dialog behaviour was observed on one docomo handset. | The app's instructions ("it may appear as a notification") may be wrong elsewhere. | Test on the XR Puck and any other Android device. |

---

## 5. Reproducing a bonding test

```bash
# node: fresh, advertising
tools/hcp.py /dev/cu.usbmodemXXXX --reboot | grep 'provisioning window'

# phone: clear stale pairing state, keep the screen awake
adb -s <serial> shell svc bluetooth disable && sleep 4
adb -s <serial> shell svc bluetooth enable  && sleep 6
adb -s <serial> shell svc power stayon true

# run the probe; accept the pairing request when the phone asks
cd magnet_app && flutter run -d <serial> -t lib/tool/ble_probe_main.dart
```

Watch the node console (`tools/hcp.py … --watch 300`) for the verdict:
`# ble: link encrypted (enc_change status=0)` means bonded;
`status=13` means the prompt was never answered.

`tools/android-pair.sh` attempts to accept the request via adb. **It does not
work** — it finds the notification but cannot invoke the action's
PendingIntent. Kept because the detection half is useful, and as a record that
this approach is a dead end.
