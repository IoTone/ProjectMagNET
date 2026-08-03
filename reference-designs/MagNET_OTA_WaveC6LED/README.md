# MagNET OTA client — Waveshare ESP32-C6-LCD-1.47 ("WaveC6LED")

Device-side OTA check-in client for [RobotARme](https://codentea.iotj.cc/robotARme).
Closes the loop that the operator console has been missing: RobotARme can upload a
release, group devices and deploy, but until this exists no device ever *pulls*
one.

Plan and phasing: `docs/wavec6led-ota-plan.md` in the RobotARme repo.

**Status: D0–D4 complete. The OTA loop closes, unattended, on hardware.**

From a cold boot, with nobody typing anything:

    update available: release 2362 version 1.1.0
    [ota] fetching + verifying...
    ota_verify: release 2362 verified: 231 bytes, sha256 ok, Ed25519 ok
    ota_apply:  applying release 2362 (231 bytes) at dict=69
    ota_apply:  apply OK: now running release 2362 version 1.1.0
    ota_apply:  apply-result reported: release 2362 OK
    [checkin]   up to date          <- converged; it stops asking

And the failure path, which matters more. A bundle whose signature is VALID but
whose Forth is broken:

    ota_verify: release 2507 verified: 201 bytes, sha256 ok, Ed25519 ok
    ota_apply:  apply FAILED, rolled back to dict=69 code=0
                — failed at: : BAD-WORD THIS-WORD-DOES-NOT-EXIST ;
    ota_apply:  apply-result reported: release 2507 FAILED

Verified afterwards: `GOOD-WORD`, defined on the line *before* the failure, is
gone — `? GOOD-WORD`. A partial apply leaves no trace. And the server's
`current_version` stayed at the last GOOD release, so a failed update cannot
make a device look upgraded.

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

## The display: use the vendor driver

**Waveshare do not use esp_lcd's generic ST7789 for this board, and neither
should you.** Their ESP-IDF demo ships its own `Vernon_ST7789T`, because the
glass is an ST7789**T** variant wanting a different init sequence — its own
porch, power and gamma tables, plus `0x21` (inversion) issued during init.
That driver is vendored into `components/magnet_ui/` here.

Settings that came from their demo rather than from guessing:

| | |
|---|---|
| driver | `esp_lcd_new_panel_st7789t`, **not** `..._st7789` |
| colour order | `LCD_RGB_ENDIAN_BGR` (RGB swaps red and blue) |
| orientation | portrait 172x320, `mirror(true, false)` |
| offset | `set_gap(34, 0)` |
| SPI clock | 12 MHz |

Inversion is already inside the vendor init, so do **not** also call
`esp_lcd_panel_invert_color()` — doing both cancels out and looks like a fresh
bug.

Getting the demo:

    curl -O https://files.waveshare.com/wiki/ESP32-C6-LCD-1.47/ESP32-C6-LCD-1.47-Demo.zip
    unzip -q ESP32-C6-LCD-1.47-Demo.zip 'ESP-IDF/*'

## THE trap: esp_lcd draws ASYNCHRONOUSLY

`esp_lcd_panel_io_tx_color()` **queues** the SPI transfer and returns. If you
reuse a scratch buffer for the next blit without waiting, the panel receives a
mixture of both.

This is worth its own section because of how it presents. Solid fills are
**immune by accident** — every band of a rectangle fill holds the same colour,
so clobbering the buffer mid-transfer writes identical bytes. So the screen
shows perfect layout, bars and boxes, and *only text* comes out as garbage
glyphs. That reads unmistakably as a font bug, and costs you an audit of the
glyph table, the renderer and the panel orientation before you suspect the
transfer. All three were fine here.

The fix is an `on_color_trans_done` callback giving a semaphore, and one
internal `blit()` that every write in `panel_st7789.c` goes through, so no
caller can forget to wait.

A useful check while chasing this: render the glyph loop to ASCII **on the
host**. If the host output is a correct letter, the bug is below the renderer,
which immediately rules out the font and the code you would otherwise stare at.

## Two other things that will bite you

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
