# Device profile — Milk-V Duo 256M (SG2002)

**Status:** 🔬 **BENCH-VALIDATED, standalone hive membership still blocked.** Profiled
2026-08-05; first power-on 2026-08-07. Board boots, camera captures and streams —
see [§3.1](#31-bench-confirmed-figures-2026-08-07) and [§3.2](#32-camera-bring-up--the-working-procedure-validated-2026-08-07).
No MagNET code written yet.
**Candidate role:** Role 11 — *Eye* ("anything that can see, capture, scan, or otherwise take a picture or video").
**Blocker:** no onboard wireless of any kind — being addressed via an **M5Stamp C3U sidecar**
(Stamp is the hive member and carries the radio; Duo is its retina over `ttyS1`).
**Conforms to:** [UDM-MagNET v1.0](../../../specs/UDM-MagNET-v1.md).

### Bench log

| Date | Outcome |
|---|---|
| 2026-08-05 | Desk research; shelved on the wireless blocker. |
| 2026-08-07 | First boot. Clocks, RAM, storage, UART map and mailbox confirmed (§3.1). RVV 0.7 trap identified. Camera confirmed streaming H.264 over RTSP, captured to file with `ffmpeg` (§3.2). TDL SDK / model path researched, not yet run (§3.3). |

---

## 1. Verdict

The Duo 256M is an excellent piece of hardware for the Eye role and a poor fit for
*this* hive, for one reason: **it has no onboard wireless.** No WiFi, no BLE, no
Thread. Its out-of-the-box network presence is a USB CDC-NCM gadget at
`192.168.42.1` — a private link between the board and whatever host it's plugged
into, which the ruler is not on and which mDNS will not cross.

The MagNET hive is defined by mDNS discovery of `_magnet-ruler._tcp` plus a TCP
session on the local subnet. A node that can't see the ruler's multicast domain
isn't a hive member; it's a USB peripheral. Every workaround — USB host mode with a
WiFi dongle, or soldering magnetics and an RJ45 to the 100 Mbps PHY pads — adds
hardware, risk, and a phase of work *before* any hive code can be written.

That's the wrong shape for a fleet whose other members are provisioned over BLE in
under a minute.

Everything else about the board is genuinely attractive, which is why this profile
exists rather than a one-line "no."

### Revisit if any of these become true

1. **A Duo S is used instead.** The [Duo S](https://milkv.io/docs/duo/getting-started/duos)
   is the same SoC family with integrated **WiFi 6 / BT 5**. It removes the entire
   blocker and inherits this whole design unchanged. **If this class of device is
   wanted, the Duo S is the answer — start there.**
2. **The deployment is fixed-installation and wired.** Ethernet on the PHY pads is
   more reliable than any radio, and sidesteps the `macOS-LAN-networking.md` and
   BLE-coupling classes of problem entirely.
3. **The hive gains a non-mDNS transport** (a broker, a relay, or a bridged tunnel),
   at which point the USB-gadget link stops being disqualifying.
4. **On-device TPU inference becomes a requirement** the ESP32 fleet can't meet.
   1.0 TOPS INT8 with a hardware ISP and H.264 encoder is a large step up from
   anything currently in the hive, and may justify the networking work on its own.

---

## 2. UDM device profile

```jsonc
{
  "udm_version": "1.0",
  "udm_doc_version": "0.1.0",
  "udm_devices": [
    {
      "udm_key": "milkv-duo256m",
      "udm_uuid": "urn:device:milkv:duo256m",
      "_comment": "SHELVED 2026-08-05 — no onboard wireless. See §1 of this file.",
      "_doc": "https://milkv.io/docs/duo/getting-started/duo256m",

      "udm_vendor": "Milk-V",
      "udm_model_name": "Duo 256M",
      "udm_mktg_name": "Milk-V Duo 256M",
      "udm_type": "compute-node",
      "udm_class": ["camera", "compute-node", "linux", "npu", "riscv"],
      "udm_tags": ["shelved", "no-onboard-wireless", "lan-only", "candidate-eye"],

      "udm_chipset_details": {
        "vendor": "Sophgo",
        "type": "SG2002",
        "instruction_set": "riscv64",   // big core switchable to arm64 (Cortex-A53)
        "frequency": "1GHz",            // ⚠️ unverified on hardware
        "core_count": 3,                // heterogeneous — see §4
        "_comment": "C906 @1GHz (or A53) + C906 @700MHz FreeRTOS + 8051 in RTC domain"
      },

      "udm_memory_volatile":     { "size": "256MB", "type": "DDR3" },
      "udm_memory_non_volatile": { "size": "microSD", "type": "removable",
                                   "_comment": "SPI-NOR / SPI-NAND / eMMC also supported by the SoC" },

      "udm_os": "linux",
      "udm_os_version": "buildroot (duo-buildroot-sdk-v2)",

      "udm_sensors": ["camera-gc2083"],
      "udm_io_ports": ["usb-c", "mipi-csi-16p-0.5mm", "gpio-header", "uart", "i2c", "spi", "pwm"],
      "udm_power_input": "5V USB-C (VBUS/VSYS), or 5V pin",

      "udm_network_interfaces": [
        { "kind": "usb-gadget", "details": "CDC-NCM, default 192.168.42.1 — host-link only, does not reach the LAN" },
        { "kind": "ethernet",   "details": "100Mbps PHY on 5 solder pads; requires external magnetics + RJ45" }
        // NOTE: no wifi, no ble, no thread. This is the blocker.
      ],

      "udm_capabilities": [
        "single-frame-capture", "h264-encode", "h265-encode", "mjpeg-stream",
        "isp-hdr", "isp-3dnr", "tpu-int8-1tops", "cvimodel", "gc2083-2mp"
      ]
    }
  ]
}
```

---

## 3. Hardware reference

Sourced from the Milk-V docs plus the Sophgo SG2002 datasheet. ⚠️ = re-verify on
physical hardware before relying on it. ✅ = **confirmed on hardware 2026-08-07**
(see [§3.1 Bench-confirmed figures](#31-bench-confirmed-figures-2026-08-07)).

| | |
|---|---|
| SoC | Sophgo SG2002 — ✅ `CVITEK CHIP ID = 18` (CV181x family) |
| Big core | RISC-V C906 **or** ARM Cortex-A53 — switchable, selected by image variant. ✅ RISC-V variant confirmed: single hart, ISA `rv64imafdvcsu`, MMU `sv39`. ✅ **`clk_c906_0` = 850 MHz, not the 1 GHz the marketing states** |
| **Vector extension** | ✅ **`v` present, but it is RVV 0.7.1 (T-Head draft), NOT ratified RVV 1.0.** Upstream GCC/LLVM vector codegen produces illegal instructions here. See [§6 Risks](#6-risks). |
| Small core | RISC-V C906, runs FreeRTOS. ✅ **`clk_c906_1` = 594 MHz, not 700 MHz** — and its clock is already *enabled* on the stock image |
| UART | ✅ 4 real 16550A ports probed from DT: `ttyS0` @ `0x4140000` (**Linux console**, `console=ttyS0,115200`), `ttyS1` @ `0x4150000`, `ttyS2` @ `0x4160000`, `ttyS3` @ `0x4170000`. `base_baud = 1562500` → **1.5 Mbaud ceiling**. `/dev/ttyS4` is a phantom legacy node with no backing device. |
| Tiny core | 8051, RTC power domain, 64 KB ROM / 256 B IRAM, **≤ 8 KB firmware in SRAM mode** |
| RAM | 256 MB physical — ✅ **Linux sees only 165.3 MB.** ~90 MB is carved out for ION media buffers, the RTOS ION pool (`0x1600000` = 22 MB) and the small core. Tunable in the SDK memory map. |
| TPU | 1.0 TOPS INT8, `.cvimodel` format via TDL SDK / tpu-mlir — ✅ `/dev/cvi-tpu0` present |
| Video | ISP (HDR, 3DNR, defog, LDC) + H.264 / H.265 encode |
| Camera | 16-pin 0.5 mm MIPI CSI — CAM-GC2083, 2 MP (1920×1080) |
| **Networking** | **None onboard.** 100 Mbps Ethernet PHY on 5 solder pads (needs external magnetics + RJ45). USB 2.0 host/device. |
| Storage | microSD — Milk-V's floor is > 1 GB (image only). **Recommended: 32 GB, Class 10 / U1, A1, high-endurance.** See [SD card sizing](#sd-card-sizing). SoC also supports SPI-NOR, SPI-NAND, eMMC |
| Boot | microSD image written with balenaEtcher / Rufus; Type-C cable powers and boots |
| Default access | USB CDC-NCM gadget, `ssh root@192.168.42.1`, password `milkv` |
| Security | secure boot, secure update, encryption |
| SDK | [duo-buildroot-sdk-v2](https://github.com/milkv-duo/duo-buildroot-sdk-v2) — **Ubuntu 22.04 amd64 host only**, or the `milkvtech/milkv-duo:latest` Docker image |
| Board targets | `milkv-duo256m-musl-riscv64-sd`, `milkv-duo256m-glibc-arm64-sd` |
| Inter-core IPC | Mailbox. Linux driver at `linux_5.10/drivers/soc/cvitek/rtos_cmdqu/`, userspace via `ioctl` on `/dev/cvi-rtos-cmdqu`. Modes: `RTOS_CMDQU_SEND_WAIT` (blocking) and `RTOS_CMDQU_SEND` (fire-and-forget). 8-byte command struct: IP ID, command ID, block flag, param pointer. ✅ **Confirmed live** — `cvi_rtos_cmdqu_probe DONE`, `cvi_spinlock_init success`, node present at `/dev/cvi-rtos-cmdqu`. |

### 3.1 Bench-confirmed figures (2026-08-07)

First power-on of the board. Everything below is measured, not sourced.

**Memory — the headline correction.** `free -h` reports **165.3 MB total**, not 256 MB.
The missing ~90 MB is reserved before Linux starts: the ION pool backing VI/VPSS/VENC
frame buffers, the RTOS ION carveout (`cvi_get_rtos_ion_size ... 0x1600000` = 22 MB), and
the small core's own region. Still ~10× a bare ESP32-S3, but it means the Eye's Linux-side
budget is 165 MB, and that growing the video pipeline and growing userspace **compete for
the same DRAM**. The split is a build-time constant in the SDK memory map, so it is
tunable — but only by rebuilding, not at runtime.

**Storage.** Stock image lands as a 739 MB rootfs (218 MB used, 468 MB free) plus a
128 MB `/boot` on `mmcblk0p1`. Confirms that expanding the rootfs (P2) is a real step and
not a formality — 468 MB does not hold the TDL SDK, a model zoo, and captured imagery.

**CPU.** `/proc/cpuinfo` shows one hart, `isa: rv64imafdvcsu`, `mmu: sv39`. Single-hart is
correct — the second C906 is the FreeRTOS small core and is invisible to Linux by design.

**Clock tree** (`/sys/kernel/debug/clk/clk_summary`) — both core clocks are *below* the
published figures:

| Clock | Rate | Enabled | Note |
|---|---|---|---|
| `clk_c906_0` | **850 MHz** | yes | Big core (Linux). Marketing says 1 GHz. |
| `clk_c906_1` | **594 MHz** | yes | Small core. Docs say 700 MHz. **Clock is already on** — the small core is powered on the stock image. |
| `clk_tpu` | 700 MHz | **no** | Gated off until a process opens `/dev/cvi-tpu0`. |
| `clk_tpu_fab` | 900 MHz | **no** | TPU fabric, likewise gated. |
| `clk_cpu_axi0` | 500 MHz | yes | System interconnect. |
| `clk_cpu_gic` | 300 MHz | yes | Interrupt controller. |

Treat 850/594 as the real numbers for any timing budget. Whether they can be raised is an
SDK clock-config question, not a runtime one — there is no cpufreq governor here. The
gated TPU clocks are the expected idle state, not a fault: they come up when the TPU
runtime opens the device.

**UART — settles the sidecar wiring.** Four 16550A ports probe from the device tree:

| Port | MMIO | IRQ | Role |
|---|---|---|---|
| `ttyS0` | `0x4140000` | 16 | **Linux console** — `console=ttyS0,115200` in `/proc/cmdline`. Leave it alone. |
| `ttyS1` | `0x4150000` | 17 | **Free — the Stamp link.** |
| `ttyS2` | `0x4160000` | 18 | Free |
| `ttyS3` | `0x4170000` | 19 | Free |

`/dev/ttyS4` exists but never probed — it's one of the 5 legacy nodes the 8250 driver
pre-registers (`CONFIG_SERIAL_8250_RUNTIME_UARTS=5`) with no backing hardware. Ignore it.

Two numbers that matter for the sidecar:

- **`base_baud = 1562500` → the ceiling is 1.5 Mbaud**, not the 921600 assumed earlier.
  At 1.5 Mbaud 8N1 that's ~150 KB/s of payload, which re-prices the imagery question:
  a 15 KB thumbnail crosses in ~0.1 s, a 200 KB full-res 1080p JPEG in ~1.4 s. Full-res
  over UART becomes *tolerable* for on-demand capture, though still wrong for streaming.
- **`dw-apb-uart 4140000.serial: forbid DMA for kernel console`** — DMA is disabled for
  `ttyS0` *specifically because* it is the console. The other three keep DMA, which is
  what makes sustaining 1.5 Mbaud without dropped bytes realistic on `ttyS1`.

Still to confirm: whether UART1's TX/RX are **pinmuxed out to the 40-pin header** on the
Duo 256M, which is a separate question from the port existing. Check with
`cat /sys/kernel/debug/pinctrl/*/pinmux-pins | grep -i uart`, or the `duo-pinmux` utility
if the image ships it.

**Media stack.** Device nodes present: `cvi-tpu0`, `cvi-mipi-rx`, `cvi-vi`, `cvi-vpss`,
`cvi-ive` (image vector engine), `cvi-dwa` (dewarp), `cvi-rgn` (overlay), and
`cvi_vc_enc0..8` / `cvi_vc_dec0..8` — nine hardware encoder and decoder instances.
`/proc/cvitek/` exposes `isp`, `venc`, `jpege`, `h264e`, `h265e`, `vi`, `vpss`, `rc`, `vb`.

**Sensor driver location — confirmed userspace.** `/mnt/system/usr/lib/` holds
`libsns_gc2083.so`, `libsns_gc4653.so`, `libsns_ov5647.so`, `libsns_full.so`. The kernel
has no sensor driver and never learns the part name; the sensor is selected at runtime by
the `SENSOR0_TYPE` environment variable (`GCORE_GC2083` for the CAM-GC2083). This is why
`dmesg | grep gc2083` returns nothing on a perfectly healthy board.

**Camera does work.** `_vi_ctrl_init: sensor_0 csibdg_w_h(1920:1080)` + `init_done`
appeared when a sample app ran. But `/proc/cvitek/vi` reads back with **empty tables** once
that app exits — VI is torn down with the process. There is no persistent camera daemon;
whatever owns the pipeline owns it for its lifetime. That shapes the Eye design: the
capture process must be long-lived, or every snapshot pays full VI init.

### SD card sizing

Milk-V's "> 1 GB" is the floor for writing the image, not a working recommendation. The
card is the Duo's *only* storage — rootfs, SDK bits, models, and captured imagery all
live on it — so size it for the Eye role, not for first boot.

| Size | Verdict |
| --- | --- |
| 1–4 GB | Boots, and that's all. No room for the TDL SDK or a model zoo. |
| 8 GB | Practical minimum. Fine through P4; tight once `.cvimodel` files and tpu-mlir output land. |
| **32 GB** | **Recommended.** Rootfs + SDK + models with room left for a rolling ring of full-res JPEGs / H.265 clips the Eye keeps locally while only detections cross the UART. |
| 64 GB+ | No benefit. With 256 MB RAM the board never works a dataset that large, and the SD stack gains nothing from the extra capacity. |

Class markings that actually matter here:

- **A1, not A2.** A1 rates random IOPS, which is what makes the rootfs feel alive.
  A2's speedup depends on command queuing that the Linux SD stack generally doesn't
  drive, so A2 cards often behave *worse* than A1 on SBCs.
- **Class 10 / U1 is plenty** for sequential throughput — the ISP encodes 1080p H.265 at
  a few Mbps, nowhere near the bus.
- **High endurance is the real spec.** A camera node writing continuously is the exact
  workload that kills consumer cards. SanDisk Max Endurance or Samsung PRO Endurance
  class. This matters more than any speed rating on the label.

Capacity above 32 GB means the card ships exFAT-formatted, which is irrelevant — the
image is written raw (balenaEtcher / `dd`), replacing the partition table outright.

### Build quickstart (if revisited)

```bash
# host: Ubuntu 22.04 amd64 (or milkvtech/milkv-duo:latest under Docker)
git clone https://github.com/milkv-duo/duo-buildroot-sdk-v2
git clone https://github.com/milkv-duo/host-tools    # ~840MB, else auto-downloaded
cp -a host-tools duo-buildroot-sdk-v2/

./build.sh milkv-duo256m-musl-riscv64-sd             # one-shot
./build.sh lunch                                      # or interactive target picker
# output: install/soc_<variant>/<board>.img
```

**Architecture choice if revisited: RISC-V musl.** The README's DBOTS acronym opens
with *"Digital: they are running modern RISC architecture"*, and the small core is a
RISC-V C906 regardless of what the big core does — picking ARM would make the board
internally bilingual for no gain. TDL SDK supports both. Fall back to `glibc-arm64`
only if a userland dependency fights musl; everything planned here is C, so that's
unlikely.

**Toolchain — do not substitute.** `source envsetup.sh` from `duo-examples`. It pins
`-mcpu=c906fdv -march=rv64imafdcv0p7xthead` — note **`v0p7`**, the draft vector
extension. A distro `riscv64-linux-gnu-gcc` targets ratified RVV 1.0 and produces
binaries that assemble cleanly then trap on this silicon. `envsetup.sh` is toolchain
configuration **only**; it sets nothing camera-related.

### 3.2 Camera bring-up — the working procedure (validated 2026-08-07)

`camera-test.sh` ships in the stock image and is the fastest path to proving the
hardware. It brings up VI + ISP + H.264 and serves RTSP. **It sets its own sensor
configuration** — exporting `SENSOR0_TYPE` by hand is not required and was a red
herring.

```bash
# on the board — leave it running, the pipeline dies with the process
camera-test.sh
```

```bash
# on the laptop (NOT the board — the Duo has no display)
ffplay -fflags nobuffer -flags low_delay rtsp://192.168.42.1/h264

# better diagnostic — proves the stream independent of any player:
ffmpeg -rtsp_transport tcp -i rtsp://192.168.42.1/h264 -t 5 -c copy /tmp/duo.mp4
```

✅ **The `ffmpeg` record path is the one confirmed working on this board.**

**Diagnosing "the camera doesn't work."** The failure is almost always in *delivery*,
not capture, and these two lines in `dmesg` settle it instantly:

```
_vi_ctrl_init: sensor_0 csibdg_w_h(1920:1080)
_vi_ctrl_init: sensor_0 init_done
```

If those appear, the sensor is detected, clocked and feeding the ISP — stop reseating
the ribbon. Then, **with the test still running**, check `/proc/cvitek/vi` from a second
session: climbing `IntCnt` / `RecvPic` in the VI CHN STATUS table means capture is
healthy and the problem is purely playback. Empty tables mean nothing owns the pipeline.

Known traps, all separately reported upstream:

| Symptom | Cause |
|---|---|
| RTSP won't play in VLC | Real defect in `camera-test.sh`'s RTSP path ([SDK #98](https://github.com/milkv-duo/duo-buildroot-sdk/issues/98)) **compounded by** recent VLC builds degrading RTSP support. Two bugs wearing one costume. Use `ffplay`/`ffmpeg` with `-rtsp_transport tcp`. |
| Kernel panic as the room dims | Genuine low-light defect ([SDK #95](https://github.com/milkv-duo/duo-buildroot-sdk/issues/95)). Test in a bright room. |
| `Can't acquire VB BLK for VPSS`, `VB_POOL pool is empty` | ION carveout too small ([SDK #145](https://github.com/milkv-duo/duo-buildroot-sdk/issues/145)). Only reachable if you rebuilt and shrank `ION_SIZE`; default is 26 MB. |
| A documented binary doesn't exist | V1 vs V2 image drift — the imaging tooling changed radically and many pages still describe V1. Suspect the doc first. |
| No `gc2083` anywhere in `dmesg` | Expected. Sensor drivers are userspace `.so` files; the kernel never learns the part name. |

### 3.3 Perception — TDL SDK, models, and labels

**`camera-test.sh` contains no model.** It is capture → encode → stream, nothing more.
There is nothing in it to "retrain" or reconfigure. Detection means replacing the
program, not editing its config.

The replacement is [`duo-tdl-examples`](https://github.com/milkv-duo/duo-tdl-examples)'s
`sample_vi_od` — same camera-to-RTSP job with TPU inference inserted, publishing to the
same `rtsp://192.168.42.1/h264`:

```bash
git clone https://github.com/milkv-duo/duo-tdl-examples.git
cd duo-tdl-examples && source envsetup.sh
cd sample_vi_od && make
# then, on the board:
./sample_vi_od yolov8-person-pets pet_det_640x384.cvimodel
```

The argument pair `<model-family> <cvimodel-file>` **is** the model-switching mechanism.

**Model zoos.** [milkv-duo/tdl-models](https://github.com/milkv-duo/tdl-models) and
upstream [sophgo/tdl_models](https://github.com/sophgo/tdl_models). Options run from
`pet_det_640x384` (person / cat / dog) through yolov8n/s COCO detection to yolo11 and
yolo12 samples.

> ⚠️ **Models are chip-specific.** This board is CV181x (`CVITEK CHIP ID = 18`) → take
> the **SG200X / cv181x** builds, e.g. `yolov8n_cv181x_int8_sym.cvimodel`. A CV180X model
> (plain Duo) will not load. This is the most common wasted hour with the TDL SDK.

**Custom models.** `.pt` → ONNX via `yolov8_export.py`, then ONNX → `.cvimodel` through
**tpu-mlir**, quantized INT8 — the TPU is 1.0 TOPS INT8 and will not run float. Conversion
runs on the Ubuntu host. Keep input resolution modest (`640x384` exists rather than
`640x640` for a reason): with 165 MB of Linux RAM and the ION pool competing for the same
DRAM, a large model plus large frame buffers is exactly how you reach
`Can't acquire VB BLK for VPSS`.

**Labels are two separate things.**

1. *Terminal output* — the samples print coordinates and confidence per detection by
   default. **This is the half the Eye actually needs**: detections, not pixels, are what
   cross the UART to the Stamp.
2. *Overlay burnt into the video* — additional work you write. The hardware block is RGN
   (`/dev/cvi-rgn`), which composites onto the frame **before** the encoder, so labels
   land inside the RTSP stream. The TDL service layer wraps it:

   ```c
   CVI_TDL_Service_ObjectDrawRect(svc, &obj_meta, &frame, true, brush);
   CVI_TDL_Service_ObjectWriteText("person", x, y, &frame, r, g, b);
   ```

   ⚠️ Signatures are from the API surface, not a compile here — check the headers in your
   SDK copy. The approach is proven: someone added `CVI_TDL_Service_ObjectWriteText` to
   `sample_vi_fd.c` and got text into the RTSP output. It is simply not pre-wired in the
   stock samples.

> **Gotcha:** a `.cvimodel` emits **class indices, not names**. The string `"person"`
> lives in a lookup table in your application code. Swap to a model with a different class
> list and forget the table, and you get confidently mislabeled boxes — which reads as a
> broken model and isn't.

**Bearing on the Eye design:** `sample_vi_od` is the closest thing to the target
architecture that already exists — it is a long-lived process owning VI, running the TPU,
and emitting structured detections. `eye-detect` is a thin layer over its printed output.
The RGN overlay is demo polish; the Stamp never sees pixels.

---

## 4. Core topology — what would run where

This was the most interesting part of the evaluation and is worth preserving. The
SG2002's three cores map unusually cleanly onto the biology the project already uses.

```
   ┌─────────────────────────────────────────────────────────────┐
   │  8051  —  "the eyelid"            RTC domain, always powered │
   │  Wake gate. PIR / RTC alarm / tamper. ≤ 8 KB.               │
   │  Wakes the big core; otherwise the board sleeps.            │
   └───────────────────────────┬─────────────────────────────────┘
                               │ wake
   ┌───────────────────────────▼─────────────────────────────────┐
   │  C906 small core (FreeRTOS)  —  "the reflex arc"            │
   │  • ESPIDFORTH — the biologic layer, role bundles execute    │
   │    HERE, exactly as on every ESP32 node                      │
   │  • Hard-real-time GPIO: IR-cut filter, IR LED PWM,          │
   │    pan/tilt servo, PIR debounce                              │
   │  • Liveness watchdog over the big core                       │
   └───────────────────────────┬─────────────────────────────────┘
                               │ mailbox  /dev/cvi-rtos-cmdqu
   ┌───────────────────────────▼─────────────────────────────────┐
   │  C906 big core (Linux)  —  "the cortex"                     │
   │  • Perception: VI → ISP → VENC (H.264/JPEG), TPU inference  │
   │  • Hive membership: mDNS + TCP + HMAC + KV  (craw_hive port) │
   │  • Serving: /stream /capture /control /status, RTSP          │
   └─────────────────────────────────────────────────────────────┘
```

### Big core (Linux) — the cortex

Everything needing a filesystem, a TCP stack, or the vendor media/TPU middleware:

- **Perception.** `VI → ISP → VPSS → VENC` via the CVITEK middleware, plus a TPU
  inference branch running a `.cvimodel`. Start from
  [duo-tdl-examples](https://github.com/milkv-duo/duo-tdl-examples) — `sample_vi_fd`
  (face detection) is the closest working reference; the YOLOv8 sample is the path
  to general object detection.
- **Hive membership.** A POSIX port of `craw_hive` (§5.1).
- **Serving.** Reproduce the `M5_Hive_Camera` HTTP surface verbatim (`/stream`,
  `/capture`, `/control`, `/status`) so every existing consumer — the Flutter app,
  the WebXR dataspaces, the Vite proxy — works with no client changes. RTSP/H.264 is
  *additive*, not a replacement for MJPEG.

### Small core (FreeRTOS) — the reflex arc

**Architectural: this is where the biologic layer belongs.** The hive's premise is
that roles are signed Forth bundles, downloaded, verified, executed. If the Duo ran
its role bundle as a Linux process it would be a different kind of citizen than
every other node — special-cased in a system whose entire point is that members are
interchangeable. Running ESPIDFORTH on the FreeRTOS small core keeps the biologic
layer *identical* to an ESP32's: same interpreter, same bundle envelope, same
`forth_eval_n()` install path. Linux becomes plumbing the biologic layer calls into,
rather than a replacement for it.

The port is tractable because `components/forth/` is already a self-contained
ESP-IDF component. What changes: the console binding (UART rather than
USB-serial-JTAG), the heap source, and the FFI words. The FFI is the interesting
part — on an ESP32 `cam-snap` calls `esp32-camera` directly; here it becomes a
mailbox RPC to the big core, which owns the sensor. That indirection is invisible to
the Forth source, so `bundles/spy-snapper.forth` would run unmodified.

**Practical: deterministic I/O and liveness.** Camera pipelines wedge. A small core
that pings Linux and can blink a distress LED or assert reset turns a hung node into
a self-healing one. Servo PWM and IR-cut switching belong here rather than fighting
the Linux scheduler for jitter.

### 8051 — the eyelid

RTC domain, so it stays powered when everything else is off. Its job is the wake
gate: watch a PIR or RTC alarm, wake the big core on an event, let it capture and
publish, sleep again. That is what would make a battery-powered Eye plausible rather
than a device tethered to a wall.

With 256 bytes of IRAM and an 8 KB firmware ceiling it can only be a state machine —
which is exactly the right size for "did something move, and is it time to look."

---

## 5. Design approach — preserved for revisit

### 5.1 Port `craw_hive` to POSIX

Restructure rather than `#ifdef`:

```
components/craw_hive/
  craw_hive.h            unchanged — the contract
  craw_hive_proto.c      shared, behind a new craw_hive_port.h shim
  craw_hive_port_esp.h   esp_random / ESP_LOGx
  craw_hive_port_posix.h getrandom(2) / fprintf
  craw_hive_node.c       ESP-IDF node (unchanged)
  craw_hive_node_posix.c NEW — pthreads + BSD sockets + Avahi
```

**`craw_hive_proto.c` is nearly portable already.** Verified against the source: it
depends on exactly four ESP symbols — `esp_random()` and `ESP_LOGW/E/I` — plus cJSON
and mbedTLS, both readily available on Buildroot. That keeps **one source of truth
for the wire format**, which matters because two implementations already exist (the
C component and `scripts/fake_ruler.py`) and a third divergent one would be a
maintenance trap.

`craw_hive_node_posix.c` is a rewrite, not a port, but a mechanical one: lwIP's
socket API *is* BSD sockets, FreeRTOS tasks become pthreads, and `mdns.h` becomes
Avahi's client API. Enable `BR2_PACKAGE_AVAHI` + `avahi-utils` in Buildroot — at
256 MB the footprint is irrelevant and it saves writing an mDNS responder.

`magnet_lineages.c` is plain C and ports as-is, so the Duo could answer the
CHALLENGE/RESPONSE lineage puzzle like any other biologic.

### 5.2 Join as Role 11 `eye` — no protocol change needed

Verified: the ruler treats `role_requested` as free-form text
(`craw_hive_ruler.c:372` reads it straight out of the payload; there is no enum
validation anywhere). So `eye` works against today's ruler with zero code change.

```c
.role_requested = "eye",
.caps = { "camera", "jpeg", "h264", "rtsp", "tpu", "detect", NULL },
.chip = "SG2002",
.gen  = MAGNET_GEN_STR,
```

Two docs would want a one-line edit for consistency: the `role_requested`
enumeration in `MagNET-HiveProtocol-v1.md` §HELLO, and the roster in the main
README. Note that `M5_Hive_Camera` currently claims Role 7 `spy`; `eye` is the
distinct Role 11 and the two can coexist.

### 5.3 Publish sightings through KV, not new message types

The Eye's real output is detections. Protocol v1 has no `PUBLISH` message type — but
it has KV, and the Scribe persists it:

```
KV_PUT  eye:<mac4>:last   {"t":1754400000,"objs":[{"c":"person","n":2,"conf":0.91}],
                           "url":"http://magnet-eye-a1b2.local/capture?seq=417"}
KV_PUT  eye:<mac4>:count  417
```

**The hive carries the sighting; HTTP carries the pixels.** Forced by the 3072-byte
KV value limit, but also the right split — the Dial ruler and the Capsule Scribe
should never have a JPEG pass through them. A WebXR dataspace or the Flutter app
reads metadata from the hive and fetches the image directly. Ships with zero changes
to `craw_hive_ruler.c`, the Scribe, or `fake_ruler.py`.

### 5.4 Eye role bundle

`bundles/eye-watcher.forth`, alongside the existing three. FFI words the small core
would expose, each a mailbox RPC to Linux:

| Word | Effect |
|---|---|
| `eye-snap` | capture one JPEG, return size |
| `eye-detect` | run the cvimodel on the current frame, return object count |
| `eye-watch` | arm continuous detection at N fps |
| `eye-publish` | `KV_PUT` the latest detection summary |
| `eye-ir` | IR-cut filter / illuminator on/off (pure small-core GPIO) |

Signed with `scripts/sign_bundle.py`, distributed by the existing Path A / Path B
mechanisms. No new infrastructure.

### 5.5 Phases

| Phase | Work | Gate |
|---|---|---|
| **P0** | Flash `milkv-duo256m-musl-riscv64-sd`, ssh over CDC-NCM, build the SDK | `ssh root@192.168.42.1` works; `./build.sh` produces an image |
| **P0.5** | **Resolve networking** — USB host mode + WiFi dongle, or Ethernet pads | Duo pings the Dial ruler; `avahi-browse -a` sees `_magnet-ruler._tcp` |
| **P1** | GC2083 bring-up: VI → ISP → VENC, single JPEG to disk | valid 1920×1080 JPEG |
| **P2** | `craw_hive_port.h` shim + `craw_hive_node_posix.c`; join as `eye` | node appears in `ruler-status` on the Dial as role `eye` (validate against `fake_ruler.py` first) |
| **P3** | HTTP surface + mDNS advertise as `magnet-eye-<mac4>.local` | existing Flutter app / Vite proxy consume it unmodified |
| **P4** | TPU `.cvimodel` inference branch, `KV_PUT` sightings | Scribe holds `eye:<mac4>:last`; Dial can `kv-get` it |
| **P5** | Small core: FreeRTOS + ESPIDFORTH + mailbox FFI; install `eye-watcher.forth` via `ROLE_GRANT` | a signed bundle from the Scribe executes on the small core and drives the Linux camera |
| **P6** | 8051 wake gate, duty-cycled operation | board sleeps, PIR wakes it, capture publishes, sleeps |

P0–P4 produce a useful hive Eye. P5 is what makes it *architecturally* a peer rather
than a bolt-on. P6 is what makes it deployable on a battery.

---

## 6. Risks

1. **Networking (P0.5) — the one that shelved it.** USB host mode plus a dongle with
   an in-tree driver (RTL8188EU / RTL8821CU class), or magnetics + RJ45 on the PHY
   pads. Either way it's hardware work before any hive code runs.
2. **Vendor middleware ergonomics.** CVITEK's VI/VPSS/VENC API is large and unevenly
   documented. Start from a working `duo-tdl-examples` sample and subtract, rather
   than building up from the API docs.
3. **Buildroot host requirement.** Ubuntu 22.04 amd64 only. On an Apple-silicon Mac
   that means Docker or a VM — and the Docker image is amd64, so it runs emulated
   and slow.
4. **Two clocks, one HMAC window.** The protocol rejects messages more than ±30 s
   from the receiver's clock. A Duo with no RTC battery boots at epoch 0 and fails
   `ts_skew` every time. Linux needs NTP up *before* hive join — mirroring the
   ESP32 nodes' load-bearing "SNTP COMPLETED → hive start" ordering.
5. **Small-core Forth is a real port, not a recompile.** P5 is optional to a demo;
   if it slips the node still works, just as a Linux-native member rather than one
   running the same biologic layer as its siblings.
6. **RVV 0.7.1, not RVV 1.0.** ⚠️ Confirmed on hardware — `/proc/cpuinfo` advertises
   `v`, but the C906 implements T-Head's **draft 0.7.1** vector extension, frozen
   years before the ratified 1.0 spec. The encodings differ. Anything built with a
   stock toolchain's `-march=...v` (RVV 1.0 codegen) will assemble happily and then
   take an illegal-instruction trap on this silicon. Consequences: use the SDK's
   bundled T-Head toolchain rather than a distro `riscv64-linux-gnu-gcc`; treat any
   upstream library with RVV 1.0 hand-intrinsics (some builds of OpenCV, ffmpeg,
   OpenBLAS) as suspect. The safe default is to leave vector codegen off entirely —
   the TPU, not the vector unit, is where perception work belongs here.
7. **165 MB, not 256 MB, and the split is build-time.** Linux and the video pipeline
   draw from the same DRAM through a carveout fixed at image-build time. Adding TPU
   models and larger frame buffers pushes against a userspace budget that cannot be
   grown without rebuilding the image.
8. **VI has no persistent owner.** `/proc/cvitek/vi` empties when the capturing
   process exits — the pipeline is torn down with it. The Eye's capture path must be
   a long-lived daemon; a fork-per-snapshot design pays full sensor + ISP init on
   every frame.

---

## 7. Open questions

1. Should an Eye also act as a **second Scribe**? 256 MB and a filesystem is vastly
   more than the Capsule's 64 KB NVS partition, making it the natural home for
   bundle storage and long-term hive memory (R16/R17) — but it muddies the
   one-node-one-role model.
2. Does an Eye with a TPU want to **serve inference for other nodes**? That's compute
   advertisement ([#54](https://github.com/IoTone/ProjectMagNET/issues/54)) — a node
   advertising `caps: ["tpu"]` that peers dispatch work to. Natural fit, well beyond
   v1 scope.
3. Is **RTSP** wanted at all, given every existing consumer speaks MJPEG over HTTP?
   Probably yes for a 1080p WebXR dataspace where MJPEG bandwidth stops being
   reasonable — but additive.

---

## 8. References

### Milk-V / vendor

- [Duo 256M — board overview & specs](https://milkv.io/docs/duo/getting-started/duo256m)
- [Duo S — the WiFi 6 / BT 5 sibling](https://milkv.io/docs/duo/getting-started/duos) ← **start here if revisiting**
- [Boot — flashing a microSD image](https://milkv.io/docs/duo/getting-started/boot)
- [Setup — CDC-NCM, `ssh root@192.168.42.1`, password `milkv`](https://milkv.io/docs/duo/getting-started/setup)
- [Buildroot SDK — targets, host deps, build commands](https://milkv.io/docs/duo/getting-started/buildroot-sdk)
- [RTOS core — FreeRTOS on the small core, mailbox IPC](https://milkv.io/docs/duo/getting-started/rtoscore)
- [8051 core — RTC domain, SDCC toolchain, 8 KB limit](https://milkv.io/docs/duo/getting-started/8051core)
- [CAM-GC2083 camera](https://milkv.io/docs/duo/camera/gc2083)
- [duo-buildroot-sdk-v2](https://github.com/milkv-duo/duo-buildroot-sdk-v2) · [host-tools](https://github.com/milkv-duo/host-tools)
- [duo-tdl-examples — TPU/TDL SDK samples](https://github.com/milkv-duo/duo-tdl-examples) · [duo-examples (`envsetup.sh`, toolchain)](https://github.com/milkv-duo/duo-examples)
- [TDL SDK introduction](https://milkv.io/docs/duo/application-development/tdl-sdk/tdl-sdk-introduction) · [face detection](https://milkv.io/docs/duo/application-development/tdl-sdk/tdl-sdk-face-detection) · [YOLOv8](https://milkv.io/docs/duo/application-development/tdl-sdk/tdl-sdk-yolov8) · [YOLOv8 person-pets](https://milkv.io/docs/duo/application-development/tdl-sdk/yolov8-person-pets-detection)
- **Model zoos**: [milkv-duo/tdl-models](https://github.com/milkv-duo/tdl-models) · [sophgo/tdl_models](https://github.com/sophgo/tdl_models) — take the **SG200X / cv181x** builds
- **Known-issue trackers referenced in §3.2**: [#98 RTSP](https://github.com/milkv-duo/duo-buildroot-sdk/issues/98) · [#95 low-light panic](https://github.com/milkv-duo/duo-buildroot-sdk/issues/95) · [#145 VB pool / ION](https://github.com/milkv-duo/duo-buildroot-sdk/issues/145)

### Internal

- [`README.md`](../README.md) — hive design, role roster (Role 11 = Eye)
- [`docs/MagNET-HiveProtocol-v1.md`](MagNET-HiveProtocol-v1.md) — wire format, HELLO/KV/ROLE_GRANT
- [`docs/MagNET-RoleBundle-v1.md`](MagNET-RoleBundle-v1.md) — bundle envelope + signing
- [`docs/MagNET-Generations.md`](MagNET-Generations.md) — gen tag + lineage gate
- [`specs/UDM-MagNET-v1.md`](../../../specs/UDM-MagNET-v1.md) — the device-profile schema §2 conforms to
- [`M5_Hive_Camera/`](../../M5_Hive_Camera/) — the incumbent camera node (Role 7 `spy`); its HTTP surface is the compatibility target
- [`components/craw_hive/`](../components/craw_hive/) — the component that would need the POSIX port
