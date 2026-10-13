# ESP-IDF v5.4 efuse build collision

A persistent build failure that affects *every* MagNET project on a Mac where `M5Capsule_Hive_Scribe_Redis` has been built. Doesn't show up on a fresh machine.

> **Permanent fix landed 2026-05-25.** Every project's `platformio.ini` now pins `framework-espidf @ 3.50301.0` via a shared `magnet-common.ini` (in the parent directory). Builds resolve to the cached, coherent v5.3.1 versioned install (`~/.platformio/packages/framework-espidf@3.50301.0/`) regardless of what the corrupted bare `framework-espidf` symlink is currently doing. Section §"Permanent fix" below covers the architecture; the rest of this doc is kept as background for understanding the original incident.

## Symptoms

```
*** Multiple ways to build the same target were specified for:
    .pio/build/esp32c3/esp_efuse_fields.c.o
    (from ['…/framework-espidf/components/efuse/esp32c3/esp_efuse_fields.c']
     and from ['…/framework-espidf/components/efuse/src/esp_efuse_fields.c'])
========================== [FAILED] Took 1.63 seconds ==========================
```

Same error on any ESP32-Cx (`esp32c2`, `esp32c3`, `esp32c5`, `esp32c6`, `esp32c61`, `esp32h2`) and likely on `esp32s2`/`esp32s3`/`esp32p4` — every chip-specific folder under `efuse/` has the same duplicated file.

## Diagnosis

Every MagNET project pins `platform = espressif32@6.9.0` (which ships ESP-IDF v5.3.1). Each project's `dependencies.lock` declares `idf: 5.3.1`. But the installed framework-espidf can silently be **v5.4.0** — confirmable with:

```bash
cat ~/.platformio/packages/framework-espidf/version.txt
grep -E "IDF_VERSION_MAJOR|IDF_VERSION_MINOR" \
  ~/.platformio/packages/framework-espidf/components/esp_common/include/esp_idf_version.h
```

The framework was upgraded in place — almost certainly when building `M5Capsule_Hive_Scribe_Redis`. The `espressif/bmi270` managed component transitively pulls in `sensor_hub → i2c_bus → i2c_master_get_bus_handle`, which is an IDF v5.4+ API. PlatformIO accommodates by upgrading framework-espidf globally, breaking every other project that targets the same install.

The upgrade is also **structurally corrupt** in the efuse component:
- `framework-espidf/components/efuse/CMakeLists.txt` (v5.4 layout) registers `src/esp_efuse_fields.c`.
- `framework-espidf/components/efuse/esp32c3/sources.cmake` (v5.3 layout, not cleaned up by the upgrade) also lists `esp_efuse_fields.c`, which gets prefixed to `esp32c3/esp_efuse_fields.c`.

Both compile to the same `.o`. CMake refuses.

## Background

See the auto-memory entry [`project_espressif_bmi270_idf54_trap`] for the original recipe that vendored the bmi270 source into `M5Capsule_Hive_Scribe_Redis/components/` to escape this trap. The vendoring should keep v5.3.1 stable *if* nothing else re-introduces the v5.4 chain.

## Fix options

### Option A (recommended) — restore the IDF version every project declares

Nuke and re-fetch:

```bash
# Optional safety net: rename instead of delete
mv ~/.platformio/packages/framework-espidf ~/.platformio/packages/framework-espidf.bak
mv ~/.platformio/platforms/espressif32   ~/.platformio/platforms/espressif32.bak 2>/dev/null
# If there are versioned dirs:
ls -d ~/.platformio/platforms/espressif32* 2>/dev/null

# Force re-fetch from any MagNET project
cd <some_project>
rm -rf .pio
pio run -t menuconfig   # triggers the fetch without flashing; quit with Q
```

Verify the result before continuing:

```bash
cat ~/.platformio/packages/framework-espidf/version.txt        # expect: 5.3.1
ls   ~/.platformio/packages/framework-espidf/components/efuse/esp32c3/esp_efuse_fields.c  # should exist (v5.3 layout)
ls   ~/.platformio/packages/framework-espidf/components/efuse/src/esp_efuse_fields.c 2>/dev/null  # should NOT exist
```

Then rebuild a few projects:

```bash
cd M5Stamp_DigDug_BLE_Lighting && rm -rf .pio && pio run -e esp32c3
cd M5Capsule_Hive_Scribe_Redis && rm -rf .pio && pio run   # confirm bmi270 vendoring still holds
```

If `M5Capsule` re-pulls v5.4, the vendoring isn't being used — inspect `managed_components/` and `idf_component.yml` there for a stealth bmi270 / sensor_hub re-dep. Don't `rm -rf` the `.bak` directories until both builds are clean.

**Caveat:** destructive; forces a ~250 MB re-download.

### Option B — surgical patch

```bash
sed -i.bak 's|"esp_efuse_fields.c"|""|' \
  ~/.platformio/packages/framework-espidf/components/efuse/esp32c3/sources.cmake
# Repeat for any chip-specific folder you target (esp32c6, esp32s3, …)
```

Quick, no re-download. **Downsides:** modifies a global platform file, will be silently wiped on next platform package update, treats symptom not cause, has to be re-applied per chip.

### Option C — pin to v5.4 properly

Bump `platform = espressif32@>=6.10.0` (clean v5.4 baseline) and regenerate `dependencies.lock` (`pio pkg update`). Less invasive to the install but accepts two IDF baselines in the codebase, which is exactly the situation that produced the mess.

## Recommendation

**Option A.** Restores the invariant the lockfiles assert (v5.3.1 everywhere). The vendored-bmi270 work was done specifically to make this safe.

---

## Permanent fix (landed 2026-05-25)

The actual long-term solution that's now in place is **per-project pinning of `platform_packages`** via a shared base config. This decouples each project's build from the bare `framework-espidf` symlink entirely.

### Architecture

`MagNET_M5DialFiddlerCrab/magnet-common.ini` declares two base sections:

```ini
[magnet_base]
platform = espressif32@6.9.0
platform_packages =
  framework-espidf @ 3.50301.0
framework = espidf
monitor_speed = 115200
build_flags = -DESPIDFORTH_ENABLE_TESTS=0

[magnet_base_v54]
platform = espressif32@6.10.0
framework = espidf
monitor_speed = 115200
build_flags = -DESPIDFORTH_ENABLE_TESTS=0
```

Each project's `platformio.ini` does:

```ini
[platformio]
default_envs = esp32c3
extra_configs = ../magnet-common.ini

[env:esp32c3]
extends = magnet_base
board = esp32-c3-devkitc-02
; …only project-specific overrides
```

### What this fixes

`platform_packages = framework-espidf @ 3.50301.0` tells PlatformIO to resolve framework-espidf to the cached versioned dir `~/.platformio/packages/framework-espidf@3.50301.0/`. That dir is a coherent v5.3.1 install (verified `version.txt: 5.3.1` + correctly-laid-out efuse component). The bare `framework-espidf` symlink can be corrupt, can sit at v5.4 forever, can be deleted entirely — the build doesn't read it.

Any future managed-component update that wants v5.4 either fails resolution at config time (giving you a clean error) or PlatformIO downloads a *new* versioned dir without touching yours.

### Fleet status

| Fleet | IDF version | Pinned? | Projects | Note |
|---|---|---|---|---|
| `magnet_base` | v5.3.1 | yes → `framework-espidf @ 3.50301.0` | 16 of 18 | Default fleet |
| `magnet_base_v54` | v5.4 | no | 1 (`MagNET_Vitals_E4TH`) | Needs v5.4 for XIAO C6 support; no clean v5.4 cached to pin to today |
| (Arduino) | n/a | n/a | 1 (`M5StackDial-m5gfx-demo-main`) | Doesn't use ESP-IDF |

### When to update

- **Adding a new project.** Copy the `extends = magnet_base` pattern from any existing project's `platformio.ini`. Don't re-declare `platform`, `framework`, or `monitor_speed`.
- **Bumping IDF version fleet-wide.** Change `framework-espidf @ 3.50301.0` in the one base section. Every project picks it up.
- **Adding a v5.4 pin** (once a clean v5.4 versioned install is cached). Add `platform_packages = framework-espidf @ 3.50400.x` to `[magnet_base_v54]`.
- **A new project needs v5.4.** Make it `extends = magnet_base_v54`.

### Verifying any one project's resolved config

```bash
~/.platformio/penv/bin/pio project config -d <project_dir> | head -25
```

Look for `platform_packages = framework-espidf @ 3.50301.0` in the env's resolved options. If it's missing, the `extends` line is broken.

## Verifying the trap is back

Quick check before each long debug session — if these don't match, expect the next esp32c-anything build to fail:

```bash
cat ~/.platformio/packages/framework-espidf/version.txt
grep version /Users/dkords/dev/projects/iotone/ProjectMagNET/reference-designs/MagNET_M5DialFiddlerCrab/M5Stamp_DigDug_BLE_Lighting/dependencies.lock | head -3
```

The first should be `5.3.1`. The second should show `version: 5.3.1` under the `idf:` entry.
