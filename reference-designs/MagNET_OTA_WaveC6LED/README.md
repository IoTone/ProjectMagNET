# MagNET OTA client — Waveshare ESP32-C6-LCD-1.47 ("WaveC6LED")

Device-side OTA check-in client for [RobotARme](https://codentea.iotj.cc/robotARme).
Closes the loop that the operator console has been missing: RobotARme can upload a
release, group devices and deploy, but until this exists no device ever *pulls*
one.

Plan and phasing: `docs/wavec6led-ota-plan.md` in the RobotARme repo.

**Status: D0 — board bring-up. Verified on hardware.**

## The board

Waveshare ESP32-C6-LCD-1.47. Picked over the M5Stamp because it has a screen and
an RGB LED: provisioning becomes something you can *see*, not something you infer
from a log.

| | |
|---|---|
| MCU | ESP32-C6FH4, RISC-V 32-bit @160 MHz |
| Memory | 4 MB flash, 512 KB HP SRAM, no PSRAM |
| Display | 1.47" ST7789, 172×320, 262K colour |
| Radio | WiFi 6 + BLE 5 |

```
LCD    MOSI GPIO6   SCLK GPIO7   CS GPIO14   DC GPIO15   RST GPIO21   BL GPIO22
RGB    GPIO8  (addressable, WS2812 family)
TF     MISO GPIO5   MOSI GPIO6   SCLK GPIO7  CS GPIO4   (shares the LCD SPI bus)
```

**Keep the backlight at ≤50% duty.** Waveshare warn that sustained full
brightness overheats the panel and leaves permanent dark shadows, and this device
is meant to display status indefinitely — precisely the abuse pattern described.
Attention-getting is the RGB LED's job, not the backlight's.

## Build and flash

Plain ESP-IDF 5.1.1. No PlatformIO.

    . ~/esp/esp-idf/export.sh
    idf.py set-target esp32c6
    idf.py -p /dev/cu.usbmodem1101 flash

Then talk to the Forth REPL over the same USB-C port at 115200.

## Measured on hardware (2026-08-02)

    free heap:  410176 bytes        (before Forth init)
    forth ready: 307772 bytes free  (100 KB dictionary, WiFi OFF)
    binary:      139232 bytes       (87% of the app partition free)

    ok> 2 3 + .
    5
    ok> : SQUARE DUP * ;
    ok> 7 SQUARE .
    49
    ok> chip-cores .
    1

That interactive define-then-run is not a toy: redefining a word at runtime is
the primitive the entire hot-swap OTA story rests on.

**This is the first time ESPIDFORTH has been confirmed RUNNING on an ESP32-C6.**
Its README listed C6 as "Builds"; it now executes.

The 307 KB figure is the number to re-measure in D1, when WiFi comes on — that
is the plan's main memory risk, and 512 KB has to hold Forth, WiFi and eventually
TLS.

## Two things that will bite you

**Project layout.** ESPIDFORTH is PlatformIO-first, which injects `src/` as the
main component. Plain `idf.py` does not, so building it unmodified fails at the
final link with `undefined reference to app_main` after compiling everything
successfully. Here the app lives in `main/`, the layout `idf.py` expects.

**Do not set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`.** Making USB-serial-JTAG the
*only* console breaks the IDF 5.1.1 build outright:

    esp_hw_support/sleep_modes.c:153: error: 'CONFIG_ESP_CONSOLE_UART_BAUDRATE'
    undeclared

`sleep_modes.c` references that symbol unconditionally while the USB-only path
never defines it. It costs nothing to leave the console at its default: the REPL
writes through `usb_serial_jtag_write_bytes()` directly, so the USB-C port is the
working I/O path regardless, and `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`
puts `ESP_LOGx` there too.

## Layout

    main/            app_main — banner, Forth init, REPL
    components/
      forth/         ESPIDFORTH core, from MagNET_M5DialFiddlerCrab/ESPIDFORTH

`components/forth/optional/` was dropped on the way in: it wraps Arduino's
`HTTPClient` and nothing in `forth_core.cpp` references it — dead Arduino-era
code that survived the ESP-IDF port. The HTTP client this project actually needs
arrives in D2 as a native `esp_http_client` component with Forth bindings.

Note the Forth engine here is ESPIDFORTH's **stub** (core ANS word set, ~1,300
lines) rather than the full ESP32forth v7.0.8.0 engine, which is preserved
alongside it as `ESP32forth.ino` and still awaits an Arduino-dependency strip.
That is why the OTA supervisor is designed as a C component with a thin word
vocabulary: when the engine is swapped, only the registration shim moves.
