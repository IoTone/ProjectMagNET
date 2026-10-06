# Hanasu — status: ON HOLD (2026-10-06)

Work paused at a clean point. Everything below is committed; nothing is pushed.
Resume from "Next, when resumed".

## Where it stands

| Area | State | Where |
|---|---|---|
| ESP-IDF core (ESP32-C6), phases E-A..E-H | Done, HW-validated (2026-07/08) | design proposal §12.10 |
| XIAO MG24 on Zephyr, Z-A..Z-F | Done, HW-validated against the C6 (2026-09-29) | `firmware-zephyr/README.md` |
| OTA packages (`.mnpkg`), spec rev 1.4 | Done, both targets | `docs/OTA-PACKAGE.md`, tags `mnpkg-spec-v1.0`..`v1.4` |
| Admin-signed remote apply, report, revoke, CLEARS_BUNDLES | Done, HW-validated both directions | proposal §13, results §13.10 |
| Release keys (offline signing, public halves only) | Tooling done, tested with throwaway keys | `tools/keys/README.md`, `genkeys.sh`, `mcuboot_sign.py` |
| FORTH-mode BLE gap (unbonded BLE could run Forth) | Fixed, HW-verified on C6 + MG24 | commit `a84d097e` |
| Channel epochs: persistence + forward catch-up (§14 option A) | Done, HW-validated | proposal §14, commit `b268f0ad` |
| Docs site: Hanasu section (14 pages) | Written, builds clean (Hugo 0.142) | `projectmagnet.github.io` `exampleSite/content/docs/hanasu/` |
| README | Rewritten, points to the docs site | `README.md` |

## Waiting on the owner

- **Release public keys.** Generate offline with `tools/keys/genkeys.sh` and hand
  over only `ota_release.pub.pem`, `ota_release_next.pub.pem`,
  `mcuboot_release.pub.pem`. Until then every build trusts the committed DEV keys
  (`!WARN ota-dev-keys` at boot) — bench only.
- **Push.** 18 Hanasu commits on `ProjectMagNET` `dev` (from `e292a767` to
  `b268f0ad`) plus tags `mnpkg-spec-v1.0`..`v1.4`; 4 commits on
  `projectmagnet.github.io` `main` (`f84c89a`..`89b8e98`).

## Next, when resumed

1. Wire in the release public keys (`tools/keys/README.md` §2) and do a first
   release build + offline sign + cable install on one node.
2. Small doc/code mismatches the docs pages call out, not yet fixed: PBKDF2 at
   100k iterations (design says 600k), no seed-phrase word-list/normalization,
   `CMD` / `RAW` / `CHANNEL LEAVE` verbs unimplemented, `CAPS` always reports
   `transports=usbcdc`, `!PEER_LEAVE` never emitted.
3. Deferred by decision (proposal §14.6): epoch options B/C (C + flash encryption
   + secure boot is the recommendation for professional deployments); scheduled
   rotation (D). Personal/chat networks keep passphrase-only joining.
4. Earlier backlog: 50-node scale test (Milestone 4), Nekobot second persona.

## Bench state at pause

- **xray1** — XIAO ESP32C6, USB serial `F0:F5:BD:2C:F6:E8`, id `2ca44570`:
  env `esp32c6_xiao_ble_led_ota`, v0.7.0+1 in `ota_0`, default channel `magnet`
  at epoch 0, admin list empty, saved bundle `hanasu-hello`, bonded to the dev Mac.
- **MG24** — XIAO MG24 Sense, USB serial `2AA118FA`, id `959f2e62`: build dir
  `mnbleota` (BLE resident, DEV MCUboot key), v0.7.0+1, default channel at epoch 0,
  admin list empty, no bundles, bonded to the dev Mac.
- Pick serial ports by USB serial number, never by `/dev` name; the
  M5StampS3 (`34:B7:DA:55:CC:70`) that sometimes shares the hub is not part of
  this bench — don't open it.
