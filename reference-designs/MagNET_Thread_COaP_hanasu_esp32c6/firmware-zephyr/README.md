# MagNET Hanasu — Zephyr firmware (Seeed XIAO MG24)

The Hanasu node on the Silicon Labs EFR32MG24 under Zephyr. It compiles the
**unmodified** MagNET core and ESPIDFORTH straight out of `../firmware-idf/`, so
the two targets cannot drift. What differs lives here:

| Path | What |
|---|---|
| `compat/` | The FreeRTOS / `esp_*` / NVS subset those sources call, on Zephyr primitives (NVS → settings `mn/<ns>/<key>`) |
| `src/magnet_crypto_psa.c` | §11.1 crypto on PSA (Zephyr ships mbedTLS 4; the legacy ccm/hkdf/ecdsa APIs are gone) + power-on KATs |
| `src/magnet_ble_zephyr.c` | BLE-GATT HCP binding (§11.2.1) on the Zephyr BT host — same service/UUIDs as the NimBLE one |
| `src/main.c` | Bring-up (same §12.5 order as IDF) + the USART0 host link |
| `third_party/cJSON` | MIT, for the role-bundle engine |
| `tools/zephyr.p` | Pop-11 wrappers: build summary, flash, RAM ranking, fault decoder, ESP-API inventory |

Hardware-validated against the ESP32-C6 bench (2026-09-28/29): mixed mesh,
encrypted chat both ways, 50 KB Type-6 transfer sha256-identical both ways,
signed role bundles (Ed25519 + HMAC) with tamper rejection and boot re-apply,
BLE bonding/persistence/long writes with BLE and Thread running concurrently.

## Setup (once)

```sh
python3.13 -m venv ~/zephyrproject/.venv && ~/zephyrproject/.venv/bin/pip install west pyocd bleak
cd ~/zephyrproject && git clone https://github.com/zephyrproject-rtos/zephyr   # pinned: 9f0253dc (main, post-4.4)
.venv/bin/west init -l zephyr
.venv/bin/west config manifest.project-filter -- '-.*,+hal_silabs,+openthread,+mbedtls,+tf-psa-crypto,+cmsis,+cmsis_6,+segger,+zcbor,+mcuboot,+picolibc'
.venv/bin/west update --narrow -o=--depth=1
.venv/bin/pip install -r zephyr/scripts/requirements-base.txt -r zephyr/scripts/requirements-build-test.txt
.venv/bin/west blobs fetch hal_silabs          # RAIL + BT controller (MSLA)
.venv/bin/west sdk install --version 1.0.1 -t arm-zephyr-eabi -d ~/zephyr-sdk-1.0.1
```

The Silabs 802.15.4 driver is `main`-only (not in v4.4.x); stay past `a9f12d74`
(SED/SSED MIC fix, zephyr#112473).

## Build variants

Everything builds with `--sysbuild`: MCUboot + the app signed for slot0.

```sh
cd ~/zephyrproject && . .venv/bin/activate && export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1
APP=<this dir>
west build --sysbuild -b xiao_mg24 $APP -d build/mnota                                                  # Thread only
west build --sysbuild -b xiao_mg24 $APP -d build/mnprov -- -DEXTRA_CONF_FILE=ble.conf                   # + BLE provisioning-only
west build --sysbuild -b xiao_mg24 $APP -d build/mnbleota -- -DEXTRA_CONF_FILE="ble.conf;ble-resident.conf"  # companion
# add debug.conf to EXTRA_CONF_FILE for fault dumps on the console
```

App options are Kconfig (`Kconfig`), not `-D`: sysbuild forwards conf files
to the app image but silently drops arbitrary `-D` cache variables.

Flash (MCUboot + app, image marked confirmed) — stock OpenOCD has no MG24
support and pyOCD's pack index lacks the part; use the Silicon Labs Arduino
core's OpenOCD:

```sh
OO=~/Library/Arduino15/packages/SiliconLabs/tools/openocd/0.12.0-arduino1-static
west flash -d build/mnota -r openocd --openocd $OO/bin/openocd --openocd-search $OO/share/openocd/scripts
```

Or from Pop-11 (`popsession start --name mg24; popsession send --name mg24 -f tools/zephyr.p`):
`zb('mn')`, `zb_sys(dir, app, extra)`, `zfl('mn')`, `zram('mn', 15)`, `zcrash_elf(logfile, 'mndbg')`, `zinv(file)`.

## Host link (read this before writing host tools)

HCP runs on USART0, bridged to USB by the board's SAMD11:

- **The bridge forwards only while DTR is asserted** (both directions). pyserial
  asserts it by default; a DTR-low open looks exactly like a dead board.
- **It injects a 0x00 when the port opens** — the firmware drops NULs.
- **Pace long writes** (32 B / 5 ms) and never `flush()` (tcdrain can block
  forever under its backpressure). One unpaced 460-byte write once wedged the
  bridge until a USB replug.
- **Keep one line in flight: wait for each reply before the next line.**
  There is no flow control. The node receives by LDMA into a 1 KB ring (no
  USART overruns, even during flash erases or BLE radio work — measured
  0/1000 lines lost, down from 0.9 % with per-byte interrupts), but if the
  dispatcher is busy (OTA staging, OpenThread) and the host keeps streaming,
  the ring overflows. The node then says `!WARN link-rx-overrun dropped=<n>`.
  `bundle_push.py` and `ota_push.py` both pace on replies.
- It buffers nothing while the port is closed: output emitted then is lost.
- USART0 is also Zephyr's console, so console/log/shell are off in `prj.conf`.

`../tools/bundle_push.py` (signed bundles over HCP) and `../tools/ble_hcp.py`
(HCP over BLE, `--id` required) follow these rules.

## Memory (256 KB RAM — every KB is spoken for)

| | Thread | +BLE |
|---|---|---|
| Static RAM | 97.9 % | 98.8 % |
| malloc arena | 76 KB | 52 KB |
| Forth heap | 32 KB | 16 KB |
| malloc low-water (bundle re-apply + COMMIT) | 34 KB | 18.9 KB |

Knobs already turned: Forth `MAX_WORD_LEN=32` (dictionary 36→20 KB), net_buf
counts trimmed (Zephyr's IP stack only ferries frames to OpenThread), main
stack 8 KB (= IDF; boot bundle re-apply needs it), mbedTLS heap 6 KB
(deterministic ECDSA runs in software). Watch `SYSINFO` `min-ever`.

## Known differences from the C6

- Path B passphrase stretch (100k PBKDF2) takes **10.6 s** vs ~1 s — the SE
  mailbox costs ~320 µs per PSA call, so the loop hashes in software. Prefer
  Path A (`qr:` / pairing) credentials on MG24 nodes.
- `esp_reset_reason` reports Zephyr hwinfo reset-cause bits, not IDF's enum.
- No WS2812 LED (`MN_ENABLE_LED=0`); the XIAO's PA7 LED is reachable from
  Forth as `gpio` pin 7 (flat numbering: port×16 + pin).
- Bundle `BUNDLE CLEAR` once appeared not to persist across a reboot (BLE
  build, right after `CHANNEL SET`); not reproducible since — watch for it.

## OTA (Z-F)

Flash map (`boards/mg24_partitions.dtsi`, shared with MCUboot): MCUboot 48 KB
@0, slot0 712 KB @0xC000, slot1 712 KB @0xBE000, storage 64 KB @0x170000
(unchanged from the pre-MCUboot layout, so identity/bonds survive). MCUboot
runs **swap-using-move** (the default swap-using-offset expects slot1 images
one sector in, and silently ignores one written at the slot start).

**Health policy** (`src/ota.c`): a swapped-in image runs in TEST state and is
confirmed only once the node reaches READY; if it has not within 10 min it
reboots and MCUboot rolls back. Verified on hardware: swap → TEST → READY →
confirmed in ~11 s; a forced-unhealthy image (`CONFIG_MN_OTA_FORCE_UNHEALTHY`)
rolled back to the previous image with no outside help.

**Mesh OTA** — any node (a C6 works) sends a signed OTA **package**
(`.mnpkg`, normative spec `../docs/OTA-PACKAGE.md`) over Thread:

```sh
PY=~/zephyrproject/.venv/bin/python
$PY ../tools/mnpkg.py build build/X/firmware-zephyr/zephyr/zephyr.signed.bin \
    --chip efr32mg24 --board xiao-mg24 --variant dev-ble-resident \
    --version 0.7.0+2 --fw-version 0.7.0-eh --key ../tools/keys/dev_release_ed25519.pem -o node.mnpkg
$PY ../tools/ota_push.py <sender-port> <target-ml-eid> node.mnpkg --apply
# stages (!OTA staged v0.7.0+2 <sha128>), then the SENDER sends an admin-signed
# OTA APPLY and waits: !OTA_RESULT <id> accepted → target reboots, TEST →
# !OTA_REPORT <id> confirmed v0.7.0+2 (was v0.7.0+1)   — or rolled-back.
# The target must allow-list the sender's key first:  ADMIN ADD <sender PUBKEY>
# Without --apply, apply on the target itself (privileged):
#   FORTH   ota-status   ota-apply     (swap in TEST mode; confirms when healthy)
```

Remote apply is design proposal §13.3–13.5: `system/ota_apply` is bound to the
target id, the staged SHA and version, so a replay after success answers
`E_NOT_STAGED`. Health = READY **and** every saved role bundle re-applied.
`ADMIN REMOVE <n|fp>` / `ADMIN REVOKE <fp>` (signed, whole channel) take keys
back out (§13.11).

The transfer is Type 6 with meta `mnpkg:1`. The receive sink
(`mn_xfer_set_sink`) checks the 256-byte header the moment chunk 0 arrives
(spec §4 checks 1–6: format, release key, signature, target, repartition,
size) and refuses a bad package before its payload crosses the air; payload
bytes go to slot1 at package offset − 256; at the end it runs checks 7–9
(SHA-256 read back from slot1, MCUboot header, `min_running`). The sender
only gets COMPLETE after all nine pass — otherwise `!XFER_FAIL <xid>
refused:<n>`, n = the check. The shared verifier is `magnet_pkg.c`
(host-tested by `../tools/pkgtest`). Authenticity = the package's release-key
signature; MCUboot re-checks its own at boot.
**Release builds** use keys generated offline (public halves only here):
`release.conf` + `-DMN_SIGN_OFFLINE=1` + the public MCUboot key, then
`../tools/mcuboot_sign.py` on the signing machine — `../tools/keys/README.md`.
**Both the release keys (`../tools/keys/`) and MCUboot's key are DEV keys;
replace them before any node leaves the bench.**

Remote apply, downgrade refusal, `CLEARS_BUNDLES` and admin revocation are
implemented on both targets (design proposal §13, results in §13.10).
