# Draft comment for IoTone/PONY-Cyberdeck-25#7 (post manually / edit freely)

## MagNET Hanasu v2 as the mesh add-on — now with an on-device Forth control surface

Status update from the ProjectMagNET side. The OpenThread/CoAP prototype referenced above has
grown into a formal design (MagNET Hanasu v2, rev 2.2) plus a working ESP-IDF re-platform, and
it maps onto this issue's requirements as an **add-on option** for the deck:

**Shape of the add-on:** any ESP32-C6 board ($3–10 — XIAO ESP32C6, M5NanoC6, bare devkit) wired
to the deck's Arduino-compatible GPIO port (UART) or USB. The deck talks a line-based **Host
Control Protocol** (HCP): `CHAT hello`, `DM <addr> <text>`, `CHANNEL SET <cred>`, with async
traffic arriving as `!CHAT …` events — trivial to drive from Python/Node/Rust, a chat UI, or an
LLM agent. Same grammar later over USB-CDC, BLE-GATT (phones/headsets), and WebSocket.

**Requirement mapping (details in the design proposal §11.8):**
- chat 1-1 + 1-N: ✅ working today (Thread multicast channels + unicast CON)
- "name a network" config / discovery: ✅ channel = a credential; all network params derived from it
- key from a 12–24-word seed phrase: ✅ specified (rev 2.2, §11.1.2 Path C — BIP39-style, 11 bits/word,
  so 12 words ≈ 132-bit key, no KDF stretch needed on-device; kid-shareable)
- photo sharing: ⚠️ honest gap — envelope currently caps app-layer transfers at ~17 KB, and
  802.15.4 is ~10 KB/s. Direction specified (extended-transfer type, 16-bit chunk index,
  host-side re-encode to ~30–100 KB). Video files: same mechanism, but expect minutes of airtime.
- < $15 networking hardware: ✅
- no bridging required: ✅ (edge routing exists as an optional UART-bridge pattern)

**The new part — R10, ESPIDFORTH:** the node now runs a Forth engine (ESPIDFORTH, the MagNET
"Hive AI" foundation) alongside the C protocol core. One serial link, two modes: structured HCP
by default, and a `FORTH` verb that drops to a live `ok>` REPL on the radio module itself. That
means the deck (or a kid at a serial monitor) can script the node *on-device*:

```
ok> : maybe-light  s" lights on" str= if 4 gpio-set then ;
ok> ' maybe-light mn-on-cmd     \ node now reacts to mesh commands with no host attached
```

Crypto/protocol stay in audited C; Forth is the automation/scripting surface. This fits the
STEM/EDU angle of the deck: the mesh dongle is itself a programmable computer.

**Where the code is:**
- Design proposal (rev 2.2): `reference-designs/MagNET_Thread_COaP_hanasu_esp32c6/MAGNet_Protocol_DESIGN_PROPOSAL.md`
- ESP-IDF firmware (E-Phase B — plaintext mesh chat + HCP + Forth REPL, builds on IDF 5.3.1):
  `reference-designs/MagNET_Thread_COaP_hanasu_esp32c6/firmware-idf/`
- Footprint on a no-PSRAM C6: flash ~0.8 MB / 2.6 MB partition, static RAM ~101 KB / 320 KB —
  fits with room for the crypto phase.

Next milestones: on-hardware multi-node validation of the ESP-IDF build, then the crypto/identity
phase (AES-CCM + Ed25519 + seed-phrase credentials), then the BLE/WebBluetooth binding so phones
can join without any dongle-side setup.
