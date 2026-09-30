# Signing keys — DEV keys here, release keys offline

Two keys decide what firmware a node will run:

| Key | Checked by | Signs | DEV key in this repo |
|---|---|---|---|
| **OTA release key** (Ed25519, optional P-256 second) | the node, before staging (OTA-PACKAGE §6) | the 192-byte `.mnpkg` header | `dev_release_ed25519.pem` (`f0846c794440d0ea`), `dev_release_p256.pem` (`831ac185a0f68623`) |
| **MCUboot key** (ECDSA P-256), MG24 only | MCUboot, at every swap and boot | the MCUboot image | MCUboot's `root-ec-p256.pem` |

The DEV private halves are public (committed here, or shipped with MCUboot):
anyone with the repo can sign firmware for a DEV-built node. Bench builds say so
at boot — `!WARN ota-dev-keys` — and `mnpkg.py build` refuses DEV keys unless
the variant starts with `dev`.

**Nothing in this procedure puts a release private key on a build machine.**
Only public halves enter the repo and the firmware.

## 1. Generate (once, on the signing machine — not a dev box)

```sh
# OTA release key (+ optionally a second one now, for rotation: "next")
python3 mnpkg.py keygen --alg ed25519 -o ota_release.pem      --pub-out ota_release.pub.pem
python3 mnpkg.py keygen --alg ed25519 -o ota_release_next.pem --pub-out ota_release_next.pub.pem

# MCUboot key (MG24)
imgtool keygen -t ecdsa-p256 -k mcuboot_release.pem
imgtool getpub -k mcuboot_release.pem -e pem > mcuboot_release.pub.pem
```

`keygen` writes the private key mode 600 and refuses to overwrite one. Back the
private files up offline; **hand over only the three `*.pub.pem` files.**

## 2. Wire in the public halves (build machine)

```sh
# OTA key store for the firmware (both targets); refuses DEV keys
tools/mnpkg.py c-keys --release ota_release.pub.pem ota_release_next.pub.pem \
    > firmware-idf/components/magnet/include/magnet_pkg_keys_release.h
cp mcuboot_release.pub.pem tools/keys/        # committed: public only
```

Release builds select them:

- **ESP32-C6:** add `-DMN_PKG_RELEASE_KEYS=1` to the env's `build_flags`. Without
  the release header the build stops with `#error`; a header holding DEV keys
  also stops it.
- **MG24:** add `release.conf` (sets `CONFIG_MN_PKG_RELEASE_KEYS`; a plain `-D`
  does not reach the app image under sysbuild), plus the offline-signing switch:

  ```sh
  west build -p -b xiao_mg24 --sysbuild firmware-zephyr -d build/rel -- \
      -DEXTRA_CONF_FILE="ble.conf;ble-resident.conf;release.conf" \
      -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"$PWD/tools/keys/mcuboot_release.pub.pem\" \
      -DMN_SIGN_OFFLINE=1
  ```

  MCUboot is built with the public key embedded; `MN_SIGN_OFFLINE`
  (`firmware-zephyr/sysbuild.cmake`) leaves the application **unsigned**
  instead of letting sysbuild sign it with that same file. The build's
  `zephyr.signed.bin` is then unsigned (hash only) and MCUboot will not boot it.

## 3. Sign (signing machine), then install or ship

```sh
# build machine → carry job/ over
tools/mcuboot_sign.py job build/rel -o job/            # zephyr.bin/.hex + exact imgtool args + SHA-256

# signing machine
python3 mcuboot_sign.py sign job/ --key mcuboot_release.pem -o signed/    # signs, then imgtool verify
python3 mnpkg.py build signed/zephyr.signed.bin --chip efr32mg24 --board xiao-mg24 \
    --variant <name> --version <X.Y.Z+N> --fw-version <tag> --key ota_release.pem -o node.mnpkg
#   C6: mnpkg.py build firmware.bin --chip esp32c6 --board xiao-esp32c6 ... --key ota_release.pem

# back on the bench — first install over SWD (bootloader + signed app):
tools/mcuboot_sign.py install signed/ build/rel && west flash -d build/rel
# afterwards over the air:  tools/ota_push.py <sender-port> <target-ml-eid> node.mnpkg --apply
```

## 4. Rotate

The store holds two OTA keys: current and next. To retire `current`, sign
packages with `next`, ship a firmware whose store is `next` + a new next
(`c-keys --release next.pub.pem newnext.pub.pem`), and destroy the old private
key once every node runs that firmware. The MCUboot key has no second slot
here: rotating it means reflashing the bootloader over SWD.

Verified on the bench (2026-09-30, throwaway keys): a public-key-only MG24
release build booted an offline-signed app; over the air, an image signed
with MCUboot's DEV key was refused by that bootloader (the node stayed on the
old image and reported `rolled-back`) while an offline-signed image swapped in
and confirmed.

| DEV file | Algorithm | key_id |
|---|---|---|
| `dev_release_ed25519.pem` | Ed25519 (sig_alg 2, preferred) | `f0846c794440d0ea` |
| `dev_release_p256.pem` | ECDSA P-256 (sig_alg 1) | `831ac185a0f68623` |
