/*
 * magnet_ui — display + RGB LED for the Waveshare ESP32-C6-LCD-1.47.
 *
 * Deliberately NOT M5GFX: the bundled m5gfx has platform layers for
 * esp32/c3/p4/s2/s3 and no c6. Panel_ST7789 is there, the platform underneath
 * it is not. This uses ESP-IDF's own esp_lcd, which supports C6 first-class.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Portrait, as the vendor drives it: the glass is 172x320 and Waveshare's own
 * ESP-IDF demo uses exactly those numbers. */
#define UI_W 172
#define UI_H 320

/* RGB565, byte-swapped for the panel's big-endian pixel order. */
#define UI_RGB(r, g, b)  ui_rgb565((r), (g), (b))
uint16_t ui_rgb565(uint8_t r, uint8_t g, uint8_t b);

/* Wipeout-ish palette, so the device looks like the rest of RobotARme. */
#define UI_BG      ui_rgb565(0x0A, 0x0A, 0x12)
#define UI_CYAN    ui_rgb565(0x36, 0xB5, 0xFF)
#define UI_AMBER   ui_rgb565(0xFF, 0x9E, 0x2A)
#define UI_GREEN   ui_rgb565(0x5A, 0xFF, 0x8A)
#define UI_RED     ui_rgb565(0xFF, 0x4D, 0x5A)
#define UI_WHITE   ui_rgb565(0xE8, 0xF0, 0xFF)
#define UI_DIM     ui_rgb565(0x4A, 0x52, 0x60)

esp_err_t ui_init(void);
void ui_clear(uint16_t colour);
void ui_fill(int x, int y, int w, int h, uint16_t colour);

/* 5x7 font scaled by an integer factor. Lowercase folds to uppercase — the
 * glyph table stops at 'Z' and a status display has no use for descenders. */
void ui_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
int  ui_text_width(const char *s, int scale);

/* Backlight duty, PERCENT, hard-clamped to UI_BL_MAX. Waveshare warn that
 * sustained full brightness permanently shadows the panel, and this device is
 * meant to display status indefinitely. Asking for 100 gets you UI_BL_MAX. */
#define UI_BL_MAX 50
void ui_backlight(int pct);

/* Single WS2812-family LED on GPIO8. */
/* Orientation + colour diagnostic. Draws unambiguous corner markers, a 1px
 * border and a big asymmetric glyph, so ONE photograph settles mirror state,
 * rotation, the column gap and the RGB channel order at once — instead of
 * guessing at each from a picture of mirrored text. */
void ui_testcard(void);

/* Panel orientation. Call before drawing; both default false. */
void ui_set_mirror(bool mx, bool my);
void ui_set_swap_xy(bool swap);

esp_err_t led_init(void);
void led_rgb(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
