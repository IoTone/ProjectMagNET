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
# ESP32-C3 (default USB-serial-JTAG console)
pio run -e esp32c3 -t upload -t monitor

# ESP32-S3
pio run -e esp32s3 -t upload -t monitor

# ESP32 classic
pio run -e esp32 -t upload -t monitor
```

## REPL Usage

On boot you'll see a banner with memory stats, then a `ok>` prompt. Type Forth expressions and press Enter.

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

### System
- `words` list all defined words
- `test` run the built-in Forth test suite (47 assertions with timing)
- `test-ffi` run the FFI test suite (8 assertions verifying ESP-IDF API calls)
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
    main.c                    # App entry, USB driver setup, REPL task
  components/
    forth/
      CMakeLists.txt          # Forth component registration
      forth_core.h            # Public C API
      forth_core.cpp          # Stub Forth interpreter (~500 lines)
      ESP32forth.ino          # Original ESP32forth v7.0.8.0 (reference)
      ESP32forth_README.txt   # Upstream readme
      optional/               # Optional ESP32forth modules
```

## Memory Usage (ESP32-C3, no PSRAM)

| Metric | Value |
|--------|-------|
| Flash | 178 KB (17% of 1 MB partition) |
| Static RAM | 66 KB (20% of 328 KB) |
| Forth dictionary heap | 100 KB (allocated at runtime) |
| Free heap after init | ~160 KB |

## Next Steps

- Port full ESP32forth v7.0.8.0 engine from `ESP32forth.ino`, replacing the stub
- Add ESP-IDF native vocabulary words (WiFi, GPIO, I2C, SPIFFS)
- PSRAM-aware dictionary allocation for ESP32-S3
- Expand test suite with ANS Forth compliance tests
