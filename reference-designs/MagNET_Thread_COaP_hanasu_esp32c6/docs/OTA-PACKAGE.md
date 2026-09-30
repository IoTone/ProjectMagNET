# MagNET OTA package format (`.mnpkg`) — v1

**Status: normative, v1 — specified 2026-09-29, not yet implemented.**
Companion to design proposal §13 (remote apply) and `docs/EXTENDED-TRANSFER.md`
(the Type 6 transport that carries it). Keywords MUST / SHOULD / MAY as in
RFC 2119.

## 1. Why a package, not a raw image

The first MG24 implementation (Z-F) shipped a bare MCUboot image and relied on
MCUboot's signature check for authenticity. That does not carry over to the
ESP32-C6: a C6 without ESP secure boot (an irreversible eFuse decision we are
not making) boots any well-formed app image. So the node itself must decide,
before an image is ever staged, that it:

1. was released by a trusted key (**authenticity**),
2. arrived intact (**integrity**),
3. is meant for this chip and board (**targeting** — a MG24 image in a C6 slot,
   or an image without the XIAO RF-switch bring-up on a XIAO, bricks the radio),
4. is an acceptable version for this node (**version policy**).

A small signed header answers all four the same way on every platform; the
platform's own image format rides inside it unchanged.

## 2. Layout

```
+---------------------------+  offset 0
| header      256 bytes     |  fixed size, signed (§3)
+---------------------------+  offset 256
| payload     image_len     |  the platform image, byte-for-byte (§5)
+---------------------------+
```

- The header is **exactly 256 bytes** in v1. Chunk 0 of a Type 6 transfer
  (336 bytes) always contains it whole, and every payload byte lands at
  package offset − 256, which keeps flash writes 4-byte aligned.
- There is no trailer. Nothing follows the payload.
- File extension `.mnpkg`; MIME `application/vnd.magnet.ota-package`.

## 3. Header

All multi-byte integers are **big-endian** (MagNET wire convention). Reserved
bytes MUST be zero when written and MUST be verified zero when read (so a v1
reader cannot silently accept a field it does not understand).

| Off | Size | Field | Value / meaning |
|----:|-----:|-------|-----------------|
| 0 | 4 | `magic` | `4D 4E 50 4B` ("MNPK") |
| 4 | 1 | `format_version` | `1` |
| 5 | 1 | `sig_alg` | `1` = deterministic ECDSA P-256 / SHA-256 (RFC 6979) |
| 6 | 2 | `header_len` | `256` |
| 8 | 2 | `chip` | §3.1 |
| 10 | 2 | `board` | §3.1; `0` = any board of that chip |
| 12 | 1 | `image_format` | §5: `1` ESP-IDF app image, `2` MCUboot signed image |
| 13 | 1 | `flags` | bit 0 `REQUIRES_REPARTITION` (never installable over the air — §7.3); others 0 |
| 14 | 2 | reserved | 0 |
| 16 | 8 | `version` | major u8 ‖ minor u8 ‖ revision u16 ‖ build u32 — the encoding of §13.3 `system/ota_apply` |
| 24 | 8 | `min_running` | same encoding; the node's running version MUST be ≥ this (all-zero = no floor). For migrations that assume an earlier step. |
| 32 | 4 | `image_len` | payload length in bytes |
| 36 | 32 | `image_sha256` | SHA-256 over the payload |
| 68 | 8 | `build_time` | Unix seconds, int64 — informational |
| 76 | 16 | `variant` | ASCII, NUL-padded: build flavour, e.g. `ble-resident`, `thread` — informational |
| 92 | 32 | `fw_version` | ASCII, NUL-padded: the firmware's `MN_FW_VERSION`, e.g. `0.8.0-zf` — informational |
| 124 | 8 | `key_id` | SHA-256(release public key, 65-byte uncompressed point)[0:8] |
| 132 | 60 | reserved | 0 |
| 192 | 64 | `signature` | r ‖ s (32 + 32 bytes), over SHA-256(header[0:192]) |

The signature covers every header byte before it — including `image_sha256`,
so it transitively covers the payload.

### 3.1 Chip and board registry

| `chip` | Name | `image_format` | Notes |
|---:|---|---:|---|
| `0x0001` | ESP32-C6 | 1 | 4 MB and 8 MB parts (§7.3) |
| `0x0002` | EFR32MG24 | 2 | |

| `board` | Name | Chip | Why it matters |
|---:|---|---|---|
| `0x0000` | any | — | Only for images with no board-specific bring-up |
| `0x0101` | M5NanoC6 | `0x0001` | env `esp32c6` |
| `0x0102` | XIAO ESP32C6 | `0x0001` | env `esp32c6_xiao` — drives the RF switch; the plain image leaves it antenna-less |
| `0x0103` | ESP32-C6-DevKitC-1 | `0x0001` | |
| `0x0201` | XIAO MG24 (incl. Sense) | `0x0002` | RF switch via board DTS |

Registry changes are append-only; an assigned value is never reused.

## 4. Verification (receiver)

A node MUST perform these checks in this order and MUST NOT mark anything
pending unless all pass. Checks 1–6 need only the header, so a receiver MAY
run them as soon as chunk 0 arrives and abort the transfer early.

| # | Check | Failure code |
|---|---|---|
| 1 | `magic`, `format_version == 1`, `header_len == 256`, `sig_alg == 1`, reserved bytes zero | `E_PKG_FORMAT` |
| 2 | `key_id` names a key in the node's release-key store (§6) | `E_PKG_KEY` |
| 3 | `signature` verifies over header[0:192] with that key | `E_PKG_SIG` |
| 4 | `chip` == this chip; `board` == this board or `0`; `image_format` is the one this chip runs | `E_PKG_TARGET` |
| 5 | `flags.REQUIRES_REPARTITION` clear | `E_PKG_REPARTITION` |
| 6 | `image_len` == transfer length − 256, and fits the update slot (§7) | `E_PKG_SIZE` |
| 7 | SHA-256(payload as written to flash, read back) == `image_sha256` | `E_PKG_SHA` |
| 8 | the platform's own image check passes (§5) | `E_PKG_IMAGE` |
| 9 | running version ≥ `min_running` | `E_PKG_MIN` |

Only after all nine is the package **staged**. Version-downgrade policy
(`version` > running unless `ALLOW_DOWNGRADE`) is enforced at **apply** time
(§13.3 check 6), not staging, so an admin can stage a known-good older build
ahead of an emergency rollback.

A staged package is identified everywhere — `system/ota_apply`, `!OTA`
events, tooling — by **`image_sha256[0:16]`** (the `sha128` of §13.3).

Check 7 hashes what was *read back from flash*, not what arrived: that also
catches a bad flash write.

## 5. Payload formats

### 5.1 `image_format = 1` — ESP-IDF app image (ESP32-C6)

The unmodified `firmware.bin` produced by the IDF build (`esp_image_header_t`
magic `0xE9`, segments, the IDF-appended SHA-256). It is written with
`esp_ota_begin(update_partition, image_len)` → `esp_ota_write_with_offset()`
(arbitrary order — Type 6 delivers out of order) → `esp_ota_end()`, which
validates the image structure and its appended hash (check 8).

### 5.2 `image_format = 2` — MCUboot signed image (EFR32MG24)

The `zephyr.signed.bin` produced by sysbuild (image header magic
`0x96f3b83d`, TLVs including MCUboot's own ECDSA-P256 signature), **unpadded**:
the update-slot trailer is written by `boot_request_upgrade()` at apply time,
never shipped. Check 8 = the MCUboot header parses and its size fields are
consistent. MCUboot then re-verifies its own signature at boot — defence in
depth on this platform; on the C6 the package signature is the *only*
authenticity check, which is why it exists.

## 6. Release keys

- Curve/algorithm: ECDSA P-256, deterministic (RFC 6979), SHA-256 — the same
  primitive as node identity (§11.1.7 as implemented), so both platforms
  verify with code they already carry (`mn_verify`).
- Nodes carry a **compiled-in store of up to 2 release keys** (current + next),
  `ota_release_keys.h`, each stored with its `key_id`. Rotating keys = ship an
  image, signed by the current key, whose store contains the next one.
- The release key is **distinct from** admin keys (§11.1.7, who may *apply*)
  and from the MCUboot key (§5.2, what may *boot*). Holding one grants none of
  the others.
- A **DEV** release key (private half committed, like `dev_ed25519.key` for
  bundles) exists for the bench and MUST be absent from the store of any image
  deployed off the bench. `mnpkg.py` refuses to build a non-`dev` variant with
  it.

## 7. Transport and platform bindings

### 7.1 Carrying a package

A package travels as one Type 6 transfer (`docs/EXTENDED-TRANSFER.md`), CON
unicast, with **meta `mnpkg:1`**. A receiver with an OTA sink claims the
transfer by that meta and writes payload bytes (package offset ≥ 256) to the
update slot at offset − 256; header bytes are kept in RAM. The receiver sends
`XC_COMPLETE` **only if §4 checks 1–9 passed** and `XC_REFUSED` otherwise, so
the sender's `!XFER_SENT` means "staged and verified" (proposal §13.5).

The v0 MG24 transport (bare image, meta `ota:<sha128>`) is superseded and is
removed when v1 lands.

### 7.2 EFR32MG24 (Zephyr)

Update slot = MCUboot slot1 (712 KB; payload ≤ 712 KB − 16 KB).
Apply = `boot_request_upgrade(TEST|PERMANENT)` + reboot; health =
confirm-when-READY / 10 min / roll back (proposal §13.1).

### 7.3 ESP32-C6 (ESP-IDF)

Update slot = the inactive `ota_N` app partition. **New partition table
(required once, over USB — a table cannot be changed over the air):**

```
# 4 MB parts (M5NanoC6, XIAO ESP32C6) — current image is 852,000 B (70 % of a slot)
nvs,      data, nvs,      0x9000,   0x6000,
otadata,  data, ota,      0xf000,   0x2000,
phy_init, data, phy,      0x11000,  0x1000,
ota_0,    app,  ota_0,    0x20000,  0x130000,   # 1216 KB
ota_1,    app,  ota_1,    0x150000, 0x130000,   # 1216 KB
scripts,  data, littlefs, 0x280000, 0x180000,   # 1.5 MB (was 960 KB)
```

8 MB parts keep the 2 × 2 MB table in `firmware-idf/README.md`.
Apply = `esp_ota_set_boot_partition()` + reboot, with
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; health = the same
confirm-when-READY policy via `esp_ota_mark_app_valid_cancel_rollback()`;
an unconfirmed image is reverted by the bootloader on the next reset — the
direct analogue of MCUboot TEST mode.

A package whose build changed the partition table MUST set
`REQUIRES_REPARTITION`; such packages are USB-only.

## 8. Tooling — `tools/mnpkg.py`

```
mnpkg.py build   --chip esp32c6 --board xiao-esp32c6 --variant thread \
                 --version 0.8.0+1 --fw-version 0.8.0-zf \
                 --key release.pem  firmware.bin  -o node.mnpkg
mnpkg.py inspect node.mnpkg                 # decode + validate structure
mnpkg.py verify  node.mnpkg --pubkey rel.pub  # §4 checks 1-8 offline
```

`build` derives `image_format` from `chip`, sets `build_time`, refuses a
payload whose own format check (§5) fails, and refuses the DEV key for a
non-`dev` variant. `ota_push.py` accepts only `.mnpkg` once v1 lands.

## 9. Worked example (header of a MG24 package)

```
00  4D 4E 50 4B 01 01 01 00  00 02 02 01 02 00 00 00   MNPK v1 sig=1 len=256 chip=MG24 board=XIAO fmt=MCUboot
10  00 08 00 00 00 00 00 01  00 07 00 00 00 00 00 00   version 0.8.0+1   min_running 0.7.0+0
20  00 06 1A 2F [image_sha256 ................ 32 B]   image_len 399,919
44  [build_time 8] [variant "ble-resident" 16]
5C  [fw_version "0.8.0-zf" 32]
7C  [key_id 8] [reserved 60 x 00]
C0  [signature r 32][signature s 32]
```

## 10. Versioning this format

A reader MUST reject `format_version` ≠ 1. A future v2 MAY change
`header_len`; v1 readers reject it at check 1, which is the intended failure.
Fields are never repurposed; new ones take reserved bytes and a new version.
