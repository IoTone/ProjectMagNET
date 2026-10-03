# MagNET Hanasu

Encrypted chat and machine-to-machine messaging over a self-healing **Thread**
mesh (IEEE 802.15.4 / 6LoWPAN / IPv6, CoAP), on ESP32-C6 and Seeed XIAO MG24
boards. Any host drives a node over USB serial, UART or BLE with one line
protocol (HCP), or drops into an embedded Forth. Firmware updates travel over
the mesh as signed packages, are applied by an admin, and roll back on their own
if the new image is unhealthy.

**Documentation:** https://projectmagnet-github-docs.pages.dev/docs/hanasu/ —
start there for concepts, getting started, the HCP reference, security, the
wire protocol and the OTA operator guide.

Current firmware: **0.7.0-eh** (ESP32-C6 and XIAO MG24).

## Repository layout

| Path | What |
|---|---|
| `firmware-idf/` | ESP32-C6 firmware (ESP-IDF via PlatformIO). `components/magnet/` is the shared MagNET core. See its README for envs, bring-up and bench notes |
| `firmware-zephyr/` | XIAO MG24 firmware (Zephyr + MCUboot); compiles the same core from `firmware-idf/` |
| `host-sdk/` | Host-side SDK (Python) |
| `tools/` | Host tools: `hcp.py`, `ble_hcp.py`, `xfer.py`, `bundle_push.py`, `ota_push.py`, `mnpkg.py` (OTA packages), `mcuboot_sign.py`, `keys/` (DEV keys + `genkeys.sh` + the release-key procedure), `pkgtest/` |
| `docs/` | Specs: `OTA-PACKAGE.md`, `EXTENDED-TRANSFER.md`, `MESH-TIME.md`, `BLE-PAIRING.md`, `BOT-MODE.md`, `NEKOBOT.md` |
| `MAGNet_Protocol_DESIGN_PROPOSAL.md` | The design: requirements, protocol, crypto, HCP, the Forth migration (§12), OTA (§13), and every phase's test record |
| `MagNET_Thread_COaP_hanasu_esp32c6.ino` | The original Arduino proof of concept (history; superseded by `firmware-idf/`) |

## Quick start

```sh
# ESP32-C6 (XIAO): build and flash
cd firmware-idf && pio run -e esp32c6_xiao -t upload
# then, on the node's serial port (115200):  CAPS · STATUS · CHAT hello
```

For the MG24, other envs (BLE, companion, OTA), channels and pairing, follow
Getting Started on the documentation site.

## Hardware

- **Seeed XIAO ESP32C6** — the bench workhorse; fits expansion boards.
- **M5Stack NanoC6** — built-in RGB LED.
- **Seeed XIAO MG24** — EFR32MG24 on Zephyr, with MCUboot.

## History

Hanasu began as an Arduino proof of concept built from the ESP32 OpenThread CoAP
lamp/switch examples, inspired by the OLPC laptop mesh and aimed at the
[PONY Cyberdeck](https://github.com/IoTone/PONY-Cyberdeck-25/issues/7):

- Milestone 1, a two-peer chat PoC — commit `59440363`.
- Milestone 2, four-peer multicast chat — commit `828648da`.

<img width="1409" height="354" alt="Arduino PoC chat" src="https://github.com/user-attachments/assets/cba88407-7927-40db-8ccb-4cedbc4e3356" />

The PoC's known limits — no application-layer security, broken DMs, 256-byte
payloads, unstable leader election — led to the design proposal and an ESP-IDF
rewrite (phases E-A to E-H, 2026-07/08), the Zephyr port to the XIAO MG24
(2026-09), and signed over-the-air updates (2026-09). The proposal records each
phase's hardware validation.

## Support

File bug reports or make PRs.
