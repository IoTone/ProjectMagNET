# ESPIDFORTH

ESP32FORTH ported to ESP-IDF via PlatformIO. Phase 2 of the MagNET Hive AI prototype.

This is a Forth interpreter running natively on ESP-IDF (not Arduino), targeting ESP32, ESP32-S3, ESP32-C3, and ESP32-C6. Currently uses a stub Forth engine implementing the core ANS Forth word set. The full ESP32forth v7.0.8.0 engine (preserved in `components/forth/ESP32forth.ino`) will replace the stub once Arduino dependencies are fully stripped.

## Supported Targets

| Target | Status | Notes |
|--------|--------|-------|
| ESP32-S3 | Builds | Primary target (M5Stack Dial, 8MB PSRAM) |
| ESP32-C3 | Builds + tested on hardware | RISC-V, no PSRAM, USB-serial-JTAG console |
| ESP32-C6 | Builds | RISC-V, no PSRAM |
| ESP32 | Builds | Classic Xtensa |

## Build and Flash

Requires PlatformIO. Select the environment matching your board:

```bash
# ESP32-C3 dev build (with test suites)
pio run -e esp32c3 -t upload -t monitor

# ESP32-C3 release build (tests stripped)
pio run -e esp32c3_release -t upload -t monitor

# ESP32-S3
pio run -e esp32s3 -t upload -t monitor

# ESP32 classic
pio run -e esp32 -t upload -t monitor
```

### Build Configurations

| Environment | Tests | Binary Size (C3) | Usage |
|-------------|-------|-------------------|-------|
| `esp32c3` | Included | 184 KB | Development |
| `esp32c3_release` | Stripped | 178 KB | Production |
| `esp32s3` | Included | — | Development |
| `esp32s3_release` | Stripped | — | Production |

Test suites are controlled by the `ESPIDFORTH_ENABLE_TESTS` flag (defined in `build_flags` in `platformio.ini`, defaults to `1`). Set to `0` in any env to strip test code. The `_release` environments do this automatically.

### Versioning

Version and build info are defined in `components/forth/forth_version.h`. The build date/time are captured at compile time via `__DATE__` and `__TIME__`.

## REPL Usage

On boot you'll see a versioned banner with build info and memory stats, then a `ok>` prompt:

```
============================================
  ESPIDFORTH v0.1.0
  Build: Apr  1 2026 12:31:45
  Phase 2: MagNET Hive AI Prototype
============================================

ESPIDFORTH v0.1.0 (build Apr  1 2026 12:31:45)
Type 'words' for vocabulary, 'bye' to exit
Test suites available: 'test', 'test-ffi'

ok>
```

Type Forth expressions and press Enter:

```
ok> 2 3 + .
5
ok> : square dup * ;
ok> 7 square .
49
ok>
```

## Available Words

### Arithmetic
`+` `-` `*` `/` `mod` `negate` `abs` `min` `max`

### Stack
`dup` `drop` `swap` `over` `rot` `nip` `tuck` `2dup` `2drop` `2swap` `depth` `pick`

### Comparison
`=` `<>` `<` `>` `<=` `>=` `0=` `0<` `0>`

### Logic
`and` `or` `xor` `invert`

### I/O
- `.` print top of stack
- `.s` show full stack contents
- `cr` emit newline
- `emit` emit character from top of stack (e.g. `65 emit` prints `A`)

### Memory
- `!` store cell (e.g. `42 addr !`)
- `@` fetch cell (e.g. `addr @`)
- `c!` store byte
- `c@` fetch byte
- `here` push current dictionary pointer
- `allot` reserve N bytes in dictionary

### Defining Words
- `: name ... ;` define a new colon word
- `variable name` create a variable
- `constant name` create a constant (e.g. `42 constant answer`)

### Control Flow
- `if ... else ... then` conditional
- `do ... loop` counted loop (e.g. `10 0 do i . loop`)
- `do ... +loop` counted loop with custom step
- `begin ... until` loop until true
- `begin ... again` infinite loop
- `begin ... while ... repeat` loop while true

### Return Stack
`>r` `r>` `r@` `i` `j`

### Number Base
- `hex` switch to hexadecimal output
- `decimal` switch to decimal output

### String
- `."` print string literal (e.g. `." hello world"`)

### Number Formats
- Decimal: `42`, `-7`
- Hexadecimal: `0xFF`
- Binary: `0b1010`

### ESP-IDF FFI Words
- `chip-model` push chip model enum (e.g. 5 = ESP32-C3)
- `chip-cores` push number of CPU cores
- `chip-rev` push chip silicon revision
- `chip-features` push chip feature bitmask
- `mac-addr` push MAC address as two cells (lo hi)
- `chip-info` print full chip info summary (model, cores, features, MAC, IDF version, free heap)

### Memory Inspection
- `mem` print full memory report (heap, Forth dict usage, stack depth, word count)
- `free-heap` push free heap size in bytes onto the stack (e.g. `free-heap .`)

### System
- `words` list all defined words
- `test` run the built-in Forth test suite — 47 assertions with timing (dev builds only)
- `test-ffi` run the FFI test suite — 8 assertions verifying ESP-IDF API calls (dev builds only)
- `bye` exit the REPL

## ESP-IDF FFI

The Forth engine can call ESP-IDF C APIs directly. Each FFI word is a thin C wrapper that calls an ESP-IDF function and pushes results onto the Forth stack. This is the same pattern used for all hardware access — adding a new FFI word is just:

1. Write a C function that pops args from the Forth stack, calls the ESP-IDF API, and pushes results
2. Register it with `add_primitive("word-name", my_function)` in `forth_init`

Example — using `chip-info` interactively:

```
ok> chip-info
Chip: ESP32-C3 rev 4, 1 core(s)
Features: WiFi BLE
MAC: 12:34:56:78:9a:bc
ESP-IDF: v5.3.1
Free heap: 198432 bytes
ok>
```

Example — using individual FFI words:

```
ok> chip-cores .
1
ok> chip-model .
5
ok> chip-rev .
4
ok>
```

## Test Suites

> Test suites are only available in dev builds (`ESPIDFORTH_ENABLE_TESTS=1`). Release builds strip all test code to save ~5.4 KB of flash. The boot banner indicates whether tests are available.

### Forth Core Tests (`test`)

Type `test` at the REPL to run 47 assertions covering arithmetic, stack ops, comparisons, logic, colon definitions, variables/constants, and control flow. Each test reports pass/fail with execution time in microseconds, plus an overall summary.

```
ok> test
=== ESPIDFORTH Test Suite ===
Arithmetic...
  PASS: add                       12 us
  PASS: sub                        8 us
  ...
Control flow...
  PASS: if true                   15 us
  PASS: do/loop                   22 us
  ...

=== Results: 47 passed, 0 failed, 47 total in 1234 us (1.2 ms) ===
ok>
```

### FFI Tests (`test-ffi`)

Type `test-ffi` to run 8 assertions that verify ESP-IDF API calls work correctly from both C and Forth. This validates the FFI bridge by calling real hardware APIs and checking the results.

```
ok> test-ffi
=== FFI Test Suite ===
  PASS: chip-model valid (5)          3 us
  PASS: chip-cores (1)                2 us
  PASS: mac-addr 12:34:56:78:9a:bc   8 us
  PASS: free-heap 198432 bytes        1 us
  PASS: idf-version "v5.3.1"         0 us
  PASS: timer-monotonic (delta 1 us)
  PASS: forth>chip-model (5)         12 us
  PASS: forth>chip-cores (1)         10 us

=== FFI Results: 8 passed, 0 failed, 8 total in 234 us (0.2 ms) ===
ok>
```

Tests 1-6 call ESP-IDF APIs directly from C. Tests 7-8 call the same APIs through the Forth interpreter (evaluating `chip-model` and `chip-cores` as Forth words), verifying the full FFI round-trip.

## Project Structure

```
ESPIDFORTH/
  platformio.ini              # PlatformIO config (espidf framework)
  CMakeLists.txt              # Top-level CMake for ESP-IDF
  sdkconfig.defaults          # Common ESP-IDF config
  sdkconfig.defaults.esp32s3  # S3-specific (PSRAM, USB console)
  sdkconfig.defaults.esp32c3  # C3-specific (USB-serial-JTAG console)
  sdkconfig.defaults.esp32c6  # C6-specific
  src/
    CMakeLists.txt            # Main component registration
    main.c                    # App entry, USB driver setup, REPL
  components/
    forth/                    # <-- Distributable component (self-contained)
      CMakeLists.txt          # ESP-IDF component registration
      idf_component.yml       # ESP Component Manager manifest
      README.md               # Component integration guide
      forth_core.h            # Public C API
      forth_core.cpp          # Forth interpreter
      forth_version.h         # Version, build info, and feature flags
      ESP32forth.ino          # Original ESP32forth v7.0.8.0 (reference)
      ESP32forth_README.txt   # Upstream readme
      optional/               # Optional ESP32forth modules
```

## Using ESPIDFORTH as a Component in Other Projects

The `components/forth/` directory is a **self-contained ESP-IDF component** that can be added to any ESP-IDF or PlatformIO project. It has no dependencies outside of standard ESP-IDF and requires only that the consuming project provide character I/O callbacks.

### Option 1: Copy the Component

Copy the `components/forth/` directory into your project:

```
your_project/
  components/
    forth/          # <-- copy this entire directory
  main/
    main.c
  CMakeLists.txt
  platformio.ini    # (if using PlatformIO)
```

### Option 2: Git Submodule

```bash
cd your_project
git submodule add <repo-url> components/forth
```

### Option 3: ESP Component Manager

Add to your project's `main/idf_component.yml`:

```yaml
dependencies:
  espidforth:
    path: ../path/to/ESPIDFORTH/components/forth
```

Or once published to the [ESP Component Registry](https://components.espressif.com/):

```yaml
dependencies:
  espidforth:
    version: ">=0.1.0"
```

### Minimal Integration Example

```c
#include "forth_core.h"

/* Provide your own character I/O — UART, USB, BLE, TCP, etc. */
static int my_getchar(void) {
    // Return a character, or -1 if none available
}

static void my_putchar(int c) {
    // Output a single character
}

void app_main(void) {
    /* Initialize with desired dictionary heap size */
    forth_init(100 * 1024);  /* 100 KB */

    /* Start interactive REPL (blocks forever until 'bye') */
    forth_repl(my_getchar, my_putchar);

    forth_deinit();
}
```

### Headless / Programmatic Usage

You can also use the Forth engine without the interactive REPL:

```c
#include "forth_core.h"

void app_main(void) {
    forth_init(100 * 1024);

    /* Evaluate Forth code programmatically */
    forth_eval(": blink 1 over gpio-set 500 ms 0 over gpio-set 500 ms ;");
    forth_eval("2 gpio-output");
    forth_eval("10 0 do 2 blink loop");

    /* Query engine state */
    int used = forth_heap_used();
    int free = forth_heap_free();

    forth_deinit();
}
```

### C API Reference

| Function | Description |
|----------|-------------|
| `forth_init(int heap_size)` | Initialize engine with dictionary heap of given size in bytes. Returns 0 on success. |
| `forth_repl(getchar, putchar)` | Run interactive REPL. Blocks until user types `bye`. Provide char I/O callbacks. |
| `forth_eval(const char *text)` | Evaluate a Forth expression string. Returns 0 on success. |
| `forth_heap_used()` | Returns bytes used in the Forth dictionary heap. |
| `forth_heap_free()` | Returns bytes free in the Forth dictionary heap. |
| `forth_deinit()` | Free all engine resources. |

### Component Files

| File | Size | Purpose |
|------|------|---------|
| `forth_core.h` | 0.9 KB | Public C API header — the only file you `#include` |
| `forth_core.cpp` | ~25 KB | Forth interpreter, FFI words, and optional test suites |
| `forth_version.h` | 0.4 KB | Version string, build date/time, feature flags |
| `idf_component.yml` | 0.2 KB | ESP Component Manager manifest |
| `CMakeLists.txt` | 0.3 KB | ESP-IDF component registration with dependencies |

### Requirements

- ESP-IDF >= 5.0 (tested with 5.1.1 and 5.3.1)
- Targets: ESP32, ESP32-S3, ESP32-C3, ESP32-C6
- ~183 KB flash, ~66 KB static RAM, ~100 KB heap (configurable)

## Memory Usage (ESP32-C3, no PSRAM)

### Static Budget (at build time)

| Layer | Size | Notes |
|-------|------|-------|
| Flash (firmware) | 183 KB | 17% of 1 MB app partition |
| Static RAM (BSS/data) | 66 KB | ESP-IDF framework + Forth globals |
| Total SRAM available | 328 KB | ESP32-C3 total |

### Runtime Budget (after boot)

| Layer | Size | Notes |
|-------|------|-------|
| Forth dictionary heap | 100 KB | `malloc`'d at boot, configurable via `FORTH_HEAP_SIZE` |
| Forth data stack | 1 KB | 256 cells x 4 bytes |
| Forth return stack | 1 KB | 256 cells x 4 bytes |
| REPL task stack | 8 KB | FreeRTOS task stack |
| Free heap remaining | ~160 KB | Available for scripts, future WiFi/BLE |

### Runtime Inspection

Use the `mem` word at any time to see current memory state:

```
ok> mem
=== Memory Report ===
  Free heap (internal): 198432 bytes
  Largest free block:   131072 bytes
  Min free ever:        195200 bytes
  Forth dict used:      1024 / 102400 bytes (1%)
  Forth stack depth:    0 / 256 cells
  Dictionary entries:   58 / 512 words
=====================
ok>
```

Other memory inspection words:
- `free-heap .` — print just the free heap bytes
- `chip-info` — includes free heap plus chip/MAC/IDF version

### Memory Notes

- On **ESP32-S3 with PSRAM**, the Forth dictionary heap is increased to 512 KB (configurable) and allocated from PSRAM, leaving internal SRAM free for WiFi/BLE stacks.
- On **ESP32-C3/C6 (no PSRAM)**, all memory comes from the 328-512 KB internal SRAM. With WiFi/BLE disabled (current config), ~160 KB remains free after the Forth engine initializes. Enabling WiFi would reduce this by ~80-100 KB.
- The "Min free ever" value in the `mem` report tracks the heap low-water mark since boot — useful for detecting if you're close to running out of memory during script execution.

## Next Steps

- Port full ESP32forth v7.0.8.0 engine from `ESP32forth.ino`, replacing the stub
- Add ESP-IDF native vocabulary words (WiFi, GPIO, I2C, SPIFFS)
- PSRAM-aware dictionary allocation for ESP32-S3
- Expand test suite with ANS Forth compliance tests
