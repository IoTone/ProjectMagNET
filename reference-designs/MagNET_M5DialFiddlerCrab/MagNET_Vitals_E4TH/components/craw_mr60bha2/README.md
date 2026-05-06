# `craw_mr60bha2`

ESP-IDF UART driver for the Seeed MR60BHA2 60GHz mmWave radar (heart rate, breathing rate, multi-target presence). C, no Arduino, no C++.

The radar runs its own DSP firmware. We only read frames over UART at 115 200 baud and decode them.

## Frame layout (Seeed Tiny Frame Interface)

```
[SOF=0x01][ID hi][ID lo][LEN hi][LEN lo][TYPE hi][TYPE lo][HEAD_CKSUM][DATA × LEN][DATA_CKSUM]
```

- Header is **big-endian** for `LEN` and `TYPE` (8 header bytes total).
- Payload values (floats, `int32_t`, `uint32_t`) are **little-endian** (matches ESP32 native order — extracted directly).
- Both checksums are `~XOR(bytes)`. The header checksum covers bytes 0..6; the data checksum covers the payload only.

## Frame types consumed

| Type   | Name                  | Payload                                                     |
|--------|-----------------------|-------------------------------------------------------------|
| 0x0A13 | HeartBreathPhase      | 3× float (total / breath / heart phase)                     |
| 0x0A14 | BreathRate            | 1× float (rpm)                                              |
| 0x0A15 | HeartRate             | 1× float (bpm)                                              |
| 0x0A16 | HeartBreathDistance   | 1× u32 range_flag, 1× float distance_m                      |
| 0x0A04 | TargetsInfo           | u32 count, count×{float x_m, float y_m, i32 dop, i32 clust} |
| 0x0A08 | TargetsPointCloud     | same shape as above                                          |
| 0x0F09 | HumanDetection        | 1× u8 present                                                |
| 0xFFFF | FirmwareVersion       | 1× u32 packed version                                        |

Anything else is logged at debug level and discarded.

## Public API

```c
#include "craw_mr60bha2.h"

esp_err_t craw_mr60_init(UART_NUM_1, /*rx*/ 17, /*tx*/ 16);

float bpm = craw_mr60_get_bpm();
bool  present = craw_mr60_get_presence();

craw_mr60_target_t targets[CRAW_MR60_MAX_TARGETS];
size_t n = craw_mr60_get_targets(targets);

uint64_t t_ms[60]; float bpm_hist[60];
size_t hist_n = craw_mr60_get_hr_history(t_ms, bpm_hist, 60);
```

`craw_mr60_get_state()` returns the full snapshot in one call when the HTTP
handler / Forth `vitals` word needs everything at once.

## Threading

A single `mr60_parse` FreeRTOS task (4 KB stack, priority 5) reads bytes,
parses frames, and updates state under a mutex. All accessors are safe
to call from any task.

## Defaults & customization

`Kconfig` exposes:

- `CONFIG_CRAW_MR60_UART_PORT` — 0 or 1, default 1
- `CONFIG_CRAW_MR60_RX_GPIO` — default 17 (XIAO C6 mmWave kit pinout)
- `CONFIG_CRAW_MR60_TX_GPIO` — default 16

These are documentation defaults; the driver itself takes pin numbers as
function arguments so embedders can override at runtime.

## Verifying against your kit

If frames aren't arriving:

1. `idf.py menuconfig` → confirm UART port + pin numbers match the kit
   schematic. The XIAO ESP32-C6 mmWave kit board pre-wires the radar
   to specific GPIOs; consult the schematic if 16/17 don't apply.
2. Set `CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y` and watch for "header cksum"
   and "data cksum" warnings — that's the parser seeing bytes but
   not finding valid frames.
3. Confirm 115 200 baud, 8N1, no flow control.
4. The radar takes ~2 seconds after power-on to start emitting.
