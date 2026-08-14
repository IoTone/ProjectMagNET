# Draft comment for IoTone/PONY-Cyberdeck-25#7 (post manually / edit freely)

## MagNET Hanasu v2 as the mesh add-on — implementation complete, hardware-validated

Status update from the ProjectMagNET side, and it's a good one: the design
(MagNET Hanasu v2, rev 2.2) is no longer a proposal with a prototype behind it —
**the full firmware phase table (E-A through E-G) is implemented and validated on
a 4-node ESP32-C6 bench** (fw 0.6.0-eg, ESP-IDF 5.3.1). Encrypted mesh chat,
seed-phrase credentials, signed admin ops, BLE phone access, on-device Forth
automation, and offline catch-up all work on real hardware today.

**Update (2026-08-11, fw 0.7.0-eh): the photo gap is closed too.** The one
requirement below that was still an honest ⚠️ — photo sharing — now has a
protocol and hardware numbers behind it. See the photo-sharing bullet.

**Shape of the add-on (unchanged):** any ESP32-C6 board ($3–10 — XIAO ESP32C6,
M5NanoC6, bare devkit) wired to the deck's Arduino-compatible GPIO port (UART) or
USB. The deck talks a line-based **Host Control Protocol** (HCP): `CHAT hello`,
`DM <addr> <text>`, `CHANNEL SET <cred>`, with async traffic arriving as
`!CHAT …` events — trivial to drive from Python/Node/Rust, a chat UI, or an LLM
agent. The same grammar rides USB-CDC, raw UART, and **BLE-GATT** (working; how
the phone app attaches), with WebSocket specified. A Python host SDK
(`host-sdk/python/`) and a Flutter app already speak it.

**Requirement mapping (details in the design proposal §11.8) — now all measured,
not promised:**
- chat 1-1 + 1-N: ✅ Thread multicast channels + unicast CON; **at chat rates the
  mesh is lossless** (0.00% measured, 1 msg/s/node single-frame, 5-min runs)
- "name a network" config / discovery: ✅ channel = a credential; selector,
  multicast group, and AES-128-CCM keys all derived from it (§11.1); nodes on
  different channels are cryptographically deaf to each other (verified both
  directions, zero MIC failures on-air)
- key from a 12–24-word seed phrase: ✅ **implemented** (§11.1.2 Path C —
  ≥12 space-separated words, HKDF, no on-device stretch; same phrase ⇒ same
  network on independent nodes, verified). The phone app generates 13-word EFF
  phrases (~134-bit) and QR credentials.
- photo sharing: ✅ **implemented and hardware-validated** (E-H, fw 0.7.0-eh).
  The v2.1 envelope's 4-bit fragment field did cap app-layer transfers at
  ~17 KB; **Type 6 extended transfer** lifts that with a 16-bit chunk index in
  the payload, unicast CON only, and window/NACK-bitmap recovery. Two-node
  over-the-air: 50 KB both directions sha256-identical at **~5.4 KiB/s**, so a
  50–100 KB host-side re-encode moves in ~10–20 s; a real-photo battery (16 /
  67 / 300 KB JPEGs, both directions, chat flowing mid-transfer, abort paths)
  passed 10/10. A transfer run *while the receiver floods the channel* still
  completed byte-identical — the loss-recovery path has fired on real radio,
  not just in theory. The node stays a modem: it never holds the file, the deck
  does. Spec: `docs/EXTENDED-TRANSFER.md`.
  Video: same mechanism, but minutes of airtime per clip — still a gap in
  practice, and the honest answer is that this link is sized for photos.
- < $15 networking hardware: ✅
- no bridging required: ✅ (edge routing stays an optional UART-bridge pattern)

**Robustness numbers from the bench** (the deck cares about these in the field):
leader node killed → survivors re-elect and re-merge in seconds-to-~2.5 min
depending on topology, chat continues, dead node rejoins automatically; nodes
that slept or powered off through traffic catch up with one CoAP
`GET /magnet/recent` poll (`RECENT` verb) — missed messages replay, duplicates
drop silently; saturation abuse (back-to-back multicast floods) degrades but
never crashes — heap flat over 5-minute overload runs.

**The R10 part — ESPIDFORTH:** the node runs a Forth engine alongside the C
protocol core. One serial link, two modes: structured HCP by default, and a
`FORTH` verb that drops to a live `ok>` REPL on the radio module itself. Scripts
persist to flash and re-arm at boot with **no host attached** — validated
end-to-end on hardware:

```
ok> : maybe-light  s" lights on" str= if 4 gpio-set then ;
ok> s" maybe-light" mn-on-cmd    \ node reacts to mesh commands on its own
```

Crypto/protocol stay in audited C; Forth never touches key material. For the
deck's STEM/EDU angle: the mesh dongle is itself a programmable computer.

**Phones join without any dongle-side setup:** a companion node keeps BLE
resident, and the Flutter app (Android-validated) provisions nodes by QR or seed
phrase, shows a live mesh view (feed backfills what happened while the phone was
away), and carries a field test console (selftest / stats / bench / stress) —
the bench scripts, in your hand.

**Where the code is:**
- Design proposal (rev 2.2, all phase logs): `reference-designs/MagNET_Thread_COaP_hanasu_esp32c6/MAGNet_Protocol_DESIGN_PROPOSAL.md`
- ESP-IDF firmware (fw 0.7.0-eh, full validation scorecards): `reference-designs/MagNET_Thread_COaP_hanasu_esp32c6/firmware-idf/`
- Photo transfer spec + scorecard: `…/docs/EXTENDED-TRANSFER.md`; host reference
  `…/tools/xfer.py`; two-pane browser demo rig `…/tools/chat-bench/`
- Host SDK + Flutter app: `…/host-sdk/`, `magnet_app/`
- Footprint on a no-PSRAM C6: flash ~39% of a 2.6 MB partition with BLE +
  crypto + Forth; ~160 KB heap free at runtime with everything on.

One spec note: signatures are **deterministic ECDSA P-256** (not Ed25519 as
earlier drafts said) — IDF 5.3.1's mbedTLS has no EdDSA, and P-256 is
hardware-accelerated on the C6.

Remaining open items are scale (32+ node soak needs hardware; everything is
sized and planned for it), multi-hop and 3+-node concurrency for the new photo
transfer (the bench is two nodes on one link), and video — which is a bandwidth
fact, not a missing feature.
