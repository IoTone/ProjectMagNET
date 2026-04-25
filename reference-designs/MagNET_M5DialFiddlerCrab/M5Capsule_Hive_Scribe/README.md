# M5Capsule Hive Scribe (Role 4)

Battery-powered ESP32-S3 hive node implementing **Role 4: Scribe** from the MagNET design — *"a scribe's only job is to save data to its internal memory and recall it from shared memory if asked."*

## Hardware

| | |
|---|---|
| Base | [M5Capsule](https://docs.m5stack.com/en/core/M5Capsule) — capsule-shaped ESP32 device with battery |
| MCU | ESP32-S3FN8 |
| Flash | 8 MB |
| PSRAM | none |
| Battery | 250 mAh LiPo |
| Console | USB-C native serial-JTAG |
| Button | G6 (WAKE) |
| Buzzer | G2 (LEDC PWM) |
| IR transmitter | G4 |
| I2C bus | SDA=G8, SCL=G40 — BMI270 IMU @ 0x69, BM8563 RTC @ 0x51 |
| microSD | SPI on G11/G12/G14/G39 |
| Mic | SPM1423 (unused in v1) |

## Why "Scribe" on the Capsule

The Capsule is the natural Scribe host because it has:
- **8 MB flash** with a 64 KB NVS partition (4× the default) — lots of room for hive shared-memory entries
- **Battery + power switch** — can be powered up briefly to "remember" something then shelved
- **microSD slot** for future overflow storage when NVS gets tight
- **RTC + IMU** — future hooks for time-stamped or motion-triggered storage
- **Tiny form factor + magnets** — sticks to a fridge as the hive's persistent memory

## Build

```bash
pio run -e m5capsule -t upload -t monitor
```

First build ≈ 9 min (full IDF + BLE/WiFi/mbedtls/mDNS/ESPIDFORTH compile). Subsequent builds are seconds. Flash via USB-C — no external adapter, no boot-mode dance.

## Sizes (verified clean build)

| | |
|---|---|
| Flash | 957 KB / 2.5 MB app partition (36.5%) |
| Static RAM | 103 KB / 320 KB (31.5%) |
| Forth heap | 32 KB |
| Steady-state RAM after BLE teardown | ~50 KB |
| NVS available for scribe entries | ~62 KB |

## Bringup flow

Mirrors the Echo / camera / ruler pattern exactly. Boot →
1. UART USB-JTAG console comes up
2. Boot chirp (1200 Hz → 1800 Hz) — buzzer sanity check
3. NVS init + WiFi event handler registered
4. BLE provisioning service starts, advertises as `MagNET-biologic-<MAC4>`
5. ESPIDFORTH REPL ready
6. If WiFi creds in NVS → auto-connect; otherwise wait for nRF Connect provisioning (same UUIDs as every other MagNET node — see [`../M5_Hive_Camera/README.md`](../M5_Hive_Camera/README.md) for the BLE characteristic table)
7. WiFi up → BLE teardown (+~55 KB) → SNTP sync → mDNS publishes `magnet-scribe-<MAC4>.local` + `_magnet-node._tcp` with TXT `role=scribe caps=scribe,kv-store`
8. Hive node joins the configured ruler, plays a 60 ms chirp at 2 kHz on JOINED state

## REPL / Forth words

Most are **interactive** — the REPL prompts for a key (max 15 chars, an NVS hard limit) and value (max 256 chars). This is friendlier than wrangling string pointers on the Forth stack at the REPL.

| Word | Stack | Description |
|---|---|---|
| `scribe-store` | ( -- ) | Prompts for key + value, writes to NVS namespace `scribe_kv` |
| `scribe-recall` | ( -- ) | Prompts for key, prints the stored string |
| `scribe-list` | ( -- ) | Prints every key=value pair in the scribe namespace |
| `scribe-erase` | ( -- ) | Prompts for key, deletes |
| `scribe-count` | ( -- n ) | Pushes the entry count to the Forth stack |
| `prov-status` | ( -- ) | BLE / WiFi / IP / SNTP / hostname / hive / scribe entry count |
| `prov-reset` | ( -- ) | Clear WiFi creds + reboot |
| `hive-status` | ( -- ) | Hive node state + session id |
| `buzz` | ( hz dur_ms -- ) | Sound the buzzer |

### Sample session

```
ok> scribe-store
key: greeting
value: Hello from the scribe
stored 'greeting' = 'Hello from the scribe'

ok> scribe-recall
key: greeting
'greeting' = 'Hello from the scribe'

ok> scribe-list
scribe entries:
  greeting        = Hello from the scribe
(1 entries)

ok> scribe-count .
1
```

## Hive role semantics

Joins as `role=scribe caps=["scribe","kv-store"]`. The Dial ruler's peer table will show this device tagged as the scribe.

For v1 the KV store is **local-only** — accessed via REPL (and in future via role bundles running on this node). The hive-wide shared-memory queries (R16 / R17 in the spec) need a new `QUERY` / `PUBLISH` message type added to the hive protocol; that work lands in **Milestone C** along with the role-bundle delivery system. When that ships, every node will be able to ask any scribe in the hive for keyed data, and the scribe will reply.

## Future hooks

Already wired but not used in v1:

- **microSD** (SPI on G11/G12/G14/G39) — overflow storage when NVS fills. NVS is 62 KB usable; SD is unbounded. Drop in `esp_vfs_fat_sdmmc_mount` and a `scribe-archive` Forth word.
- **RTC (BM8563)** — once the hive QUERY protocol is in, the scribe could timestamp every store with the RTC's wall-clock value (independent of SNTP). Lets us survive WiFi drops without losing temporal ordering.
- **IMU (BMI270)** — motion-triggered persistence ("flush write-buffer on shake"). Useful for low-power deployments.
- **IR TX (G4)** — the scribe could also broadcast key/value pairs to other devices via IR (e.g. as a light-of-sight backup channel). Out of scope for v1 but the GPIO is reserved.

## Troubleshooting

If you can't reach `magnet-scribe-<mac4>.local` from your Mac, see [`../docs/macOS-LAN-networking.md`](../docs/macOS-LAN-networking.md) — the same Local Network / Private Relay / Private MAC gotchas apply.

If BLE provisioning won't advertise: it likely auto-connected to a stored network. `prov-reset` at the REPL clears creds and reboots into provisioning mode. Or erase NVS via esptool (see `../M5Atom_Echo_Hex_Hive_Test/README.md` troubleshooting section — same recipe).

## Related

- `../M5_Hive_Camera/` — Role 7 Spy on AI-Thinker / CamS3
- `../M5Atom_Echo_Hex_Hive_Test/` — reference bringup pattern (this project's main.c follows it)
- `../M5StackDial-m5gfx-demo-ESPIDFORTH/` — the hive ruler
- `../docs/MagNET-HiveProtocol-v1.md` — on-wire spec
- `../docs/macOS-LAN-networking.md` — macOS dev gotchas
