# OTA release keys — DEV ONLY

`dev_release_ed25519.pem` and `dev_release_p256.pem` are the **bench** OTA
release keys (docs/OTA-PACKAGE.md §6). Their private halves are committed on
purpose, exactly like `dev_ed25519.key` for role bundles: anyone with this
repo can sign firmware that a DEV-store node will stage.

- The firmware's key store (`firmware-idf/components/magnet/include/magnet_pkg_keys.h`)
  currently holds these two keys and says so (`MN_PKG_KEYS_ARE_DEV 1`).
- `tools/mnpkg.py build` refuses these keys unless the variant starts with `dev`.
- Before any node leaves the bench: generate the real release key off this
  machine (`mnpkg.py keygen`), regenerate the store with
  `mnpkg.py c-keys <release.pem> [<next.pem>]`, and keep the private key out
  of the repo.

| File | Algorithm | key_id |
|---|---|---|
| `dev_release_ed25519.pem` | Ed25519 (sig_alg 2, preferred) | `f0846c794440d0ea` |
| `dev_release_p256.pem` | ECDSA P-256 (sig_alg 1) | `831ac185a0f68623` |
