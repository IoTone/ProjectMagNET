/*
 * XIAO ESP32C3 IOT Lighting — E4TH (Phase 1: port + self-test)
 *
 * Port of the campsynthesis 4-strip random-color NeoPixel sketch to the
 * ESPIDFORTH ("E4TH") runtime, on the Xiao ESP32C3 in a Grove Shield for
 * XIAO. The strips are Grove WS2813 (60 px each).
 *
 * Phase 1 drives ONE strip on the Grove D0 socket via the ESP32-C3 RMT
 * peripheral (ESP-IDF led_strip) — hardware-timed, so no bit-bang jitter.
 * WS2813 has a dual data line (DIN + backup BIN); the D0 socket exposes
 * DIN on GPIO2 (yellow) and BIN on GPIO3 (white). Both are driven with
 * identical pixels using the C3's two RMT TX channels. Driving 4 fully
 * independent strips needs >2 channels — that topology (likely chaining
 * the WS2813s into one run) is a Phase-2 decision.
 *
 * The XIAO ESP32C3 does NOT break out raw GPIO 0/1; its silkscreen pads
 * D0..D5 are GPIO 2,3,4,5,6,7. NOTE: WS2813 needs ~5V VDD — a 3.3V Grove
 * socket will not light the strip regardless of firmware.
 *
 * Phase 1 is scaffold + port + self-test only. No WiFi/BLE/HTTP — those
 * arrive in Phase 2 (BLE->WiFi provisioning + UC2 /api/v1/actuator/neopixel).
 *
 * REPL words (type at the USB-serial console):
 *   selftest       -- bars + all-on + per-strip walk (verifies wiring/order)
 *   selftest-bars  -- fill all strips R, then G, then B (channel-order check)
 *   selftest-allon -- all strips dim white for 3 s
 *   selftest-walk  -- single pixel marches down each strip in turn
 *   N randrun      -- N frames of the ported random-color pattern
 *   ws-clear       -- blank all strips
 *   strips         -- print wiring / word help
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_random.h"
#include "driver/usb_serial_jtag.h"
#include "led_strip.h"
#include "forth_core.h"
#include "forth_version.h"

#define STRIP_COUNT 4
#define NUM_PIXELS  60
#define BYTES_PER   (NUM_PIXELS * 3)
#define FORTH_HEAP_SIZE (64 * 1024)

/* Phase 1: ONE physical Grove WS2813 strip on the D0 socket, driven by the
 * ESP32-C3 RMT peripheral (hardware-timed -- no bit-bang jitter). WS2813
 * has a dual data line (DIN + backup BIN); the Grove D0 socket exposes DIN
 * on GPIO2 (yellow) and BIN on GPIO3 (white). Both must carry the data, so
 * we use the C3's two RMT TX channels: one led_strip device per pin, fed
 * identical pixels. (4 independent strips don't fit 2 RMT channels -- the
 * multi-strip topology, likely chaining, is a Phase-2 decision.)
 *
 * Logical strips 1..3 (NSTRIP stays 4 so the Forth bundle is unchanged)
 * have no hardware in Phase 1 and are no-ops on show. */
#define STRIP_DIN_GPIO 2   /* Grove D0 primary pin (yellow, WS2813 DIN) */
#define STRIP_BIN_GPIO 3   /* Grove D0 2nd pin     (white,  WS2813 BIN) */

static led_strip_handle_t s_din;   /* RMT device on DIN (GPIO2) */
static led_strip_handle_t s_bin;   /* RMT device on BIN (GPIO3) */

/* Per-strip shadow framebuffer, WS2812 wire order = G,R,B. */
static uint8_t s_fb[STRIP_COUNT][BYTES_PER];

/* Master brightness cap (0..255), applied per channel. Default 64 (~25%)
 * so a stray "full white on all strips" can't demand ~14 A on a bench PSU.
 * Raise with `255 bri` once an adequately-sized 5V supply is wired. */
static volatile uint8_t s_bri_cap = 64;

/* Active pixels per strip (1..NUM_PIXELS). Lets a partial / cut strip
 * self-test cleanly: fills and the walk only touch connected LEDs.
 * Set with `30 npx`. */
static volatile int s_active_px = NUM_PIXELS;

/* ---- USB serial I/O ---- */

static void usb_print(const char *s) {
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(500));
}

static void usb_printf(const char *fmt, ...) {
    char buf[200];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    usb_print(buf);
}

static int uart_getchar(void) {
    uint8_t c;
    int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
    if (n <= 0) return -1;
    return c;
}

static void uart_putchar(int c) {
    uint8_t ch = (uint8_t)c;
    usb_serial_jtag_write_bytes(&ch, 1, pdMS_TO_TICKS(100));
}

static void setup_usb_serial(void) {
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    usb_serial_jtag_driver_install(&cfg);
}

/* ---- WS2813 RMT driver (ESP-IDF led_strip) ---- */

static led_strip_handle_t make_rmt_strip(int gpio) {
    led_strip_config_t scfg = {
        .strip_gpio_num = gpio,
        .max_leds = NUM_PIXELS,
    };
    led_strip_rmt_config_t rcfg = {
        .resolution_hz = 10 * 1000 * 1000,   /* 10 MHz -> WS2812/13 timing */
    };
    led_strip_handle_t h = NULL;
    esp_err_t e = led_strip_new_rmt_device(&scfg, &rcfg, &h);
    if (e != ESP_OK) {
        usb_printf("[led] RMT init GPIO %d failed: %d\r\n", gpio, (int)e);
        return NULL;
    }
    led_strip_clear(h);
    return h;
}

static void ws_gpio_init(void) {
    memset(s_fb, 0, sizeof(s_fb));
    s_din = make_rmt_strip(STRIP_DIN_GPIO);
    s_bin = make_rmt_strip(STRIP_BIN_GPIO);   /* WS2813 backup line */
    if (!s_bin)
        usb_print("[led] BIN channel unavailable -- driving DIN only\r\n");
}

static inline int strip_ok(int s) { return s >= 0 && s < STRIP_COUNT; }

static inline uint8_t bri_scale(uint8_t v) {
    return (uint8_t)(((uint32_t)v * s_bri_cap) / 255U);
}

static void ws_set_pixel(int s, int i, uint8_t r, uint8_t g, uint8_t b) {
    if (!strip_ok(s) || i < 0 || i >= NUM_PIXELS) return;
    uint8_t *p = &s_fb[s][i * 3];
    p[0] = bri_scale(g); p[1] = bri_scale(r); p[2] = bri_scale(b);  /* GRB */
}

static void ws_fill(int s, uint8_t r, uint8_t g, uint8_t b) {
    if (!strip_ok(s)) return;
    for (int i = 0; i < NUM_PIXELS; i++)
        if (i < s_active_px) ws_set_pixel(s, i, r, g, b);
        else                 ws_set_pixel(s, i, 0, 0, 0);
}

static void ws_rand_fill(int s) {
    if (!strip_ok(s)) return;
    for (int i = 0; i < NUM_PIXELS; i++) {
        if (i < s_active_px) {
            uint32_t v = esp_random();
            ws_set_pixel(s, i, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        } else {
            ws_set_pixel(s, i, 0, 0, 0);
        }
    }
}

/* Push strip 0's shadow framebuffer to the RMT device(s) and latch. RMT
 * generates WS2813 timing in hardware. Phase 1 has one physical strip
 * (logical 0); other indices are no-ops. */
static void ws_show(int s) {
    if (s != 0 || !s_din) return;
    for (int i = 0; i < NUM_PIXELS; i++) {
        uint8_t *p = &s_fb[0][i * 3];   /* stored G,R,B */
        led_strip_set_pixel(s_din, i, p[1], p[0], p[2]);
        if (s_bin) led_strip_set_pixel(s_bin, i, p[1], p[0], p[2]);
    }
    led_strip_refresh(s_din);
    if (s_bin) led_strip_refresh(s_bin);
}

static void ws_show_all(void) {
    ws_show(0);
}

static void ws_clear_all(void) {
    memset(s_fb, 0, sizeof(s_fb));
    if (s_din) led_strip_clear(s_din);
    if (s_bin) led_strip_clear(s_bin);
}

/* ---- Forth FFI words ---- */

/* ( strip idx r g b -- ) */
static void w_ws_px(void) {
    uint8_t b = (uint8_t)forth_pop();
    uint8_t g = (uint8_t)forth_pop();
    uint8_t r = (uint8_t)forth_pop();
    int idx   = (int)forth_pop();
    int s     = (int)forth_pop();
    ws_set_pixel(s, idx, r, g, b);
}

/* ( strip r g b -- ) */
static void w_ws_fill(void) {
    uint8_t b = (uint8_t)forth_pop();
    uint8_t g = (uint8_t)forth_pop();
    uint8_t r = (uint8_t)forth_pop();
    int s     = (int)forth_pop();
    ws_fill(s, r, g, b);
}

/* ( r g b -- ) */
static void w_ws_fill_all(void) {
    uint8_t b = (uint8_t)forth_pop();
    uint8_t g = (uint8_t)forth_pop();
    uint8_t r = (uint8_t)forth_pop();
    for (int s = 0; s < STRIP_COUNT; s++) ws_fill(s, r, g, b);
}

/* ( strip -- ) */
static void w_ws_rand_fill(void) {
    ws_rand_fill((int)forth_pop());
}

/* ( strip -- ) */
static void w_ws_show(void) {
    ws_show((int)forth_pop());
}

/* ( -- ) */
static void w_ws_show_all(void) {
    ws_show_all();
}

/* ( -- ) */
static void w_ws_clear(void) {
    ws_clear_all();
}

/* ( n -- )  set master brightness cap 0..255 */
static void w_bri(void) {
    intptr_t n = forth_pop();
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    s_bri_cap = (uint8_t)n;
    usb_printf("\r\nbrightness cap = %d/255\r\n", (int)s_bri_cap);
}

/* ( -- n )  push current brightness cap */
static void w_bri_q(void) {
    forth_push(s_bri_cap);
}

/* ( n -- )  set active pixels per strip 1..NUM_PIXELS (partial strips) */
static void w_npx(void) {
    intptr_t n = forth_pop();
    if (n < 1) n = 1;
    if (n > NUM_PIXELS) n = NUM_PIXELS;
    s_active_px = (int)n;
    usb_printf("\r\nactive pixels/strip = %d\r\n", s_active_px);
}

/* ( -- n )  push active pixels per strip */
static void w_npx_q(void) {
    forth_push(s_active_px);
}

/* ( n -- r )  pseudo-random in [0,n) */
static void w_rnd(void) {
    intptr_t n = forth_pop();
    forth_push(n > 0 ? (intptr_t)(esp_random() % (uint32_t)n) : 0);
}

/* ( ms -- )  cooperative delay */
static void w_nap(void) {
    intptr_t ms = forth_pop();
    if (ms < 0) ms = 0;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ( -- )  print wiring + word help */
static void w_strips(void) {
    usb_print("\r\nGrove WS2813, 60 px, RMT-driven (Phase 1: 1 strip):\r\n");
    usb_print("  strip 0 -> Grove D0: GPIO2 DIN + GPIO3 BIN (2x RMT)\r\n");
    usb_print("  strips 1-3 -> no hardware in Phase 1 (no-op)\r\n");
    usb_print("  (WS2813 needs ~5V VDD; Grove socket VCC may be 3.3V)\r\n");
    usb_print("Words:\r\n");
    usb_print("  smoke           low-power: px0 of each strip dim (USB-safe)\r\n");
    usb_print("  N id            light first 3 px of strip N dim green\r\n");
    usb_print("  selftest        bars + all-on + walk\r\n");
    usb_print("  selftest-bars   R / G / B fill (channel order)\r\n");
    usb_print("  selftest-allon  all strips dim white 3 s\r\n");
    usb_print("  selftest-walk   marching pixel per strip\r\n");
    usb_print("  N randrun       N frames of random pattern\r\n");
    usb_print("  ws-clear        blank all strips\r\n");
    usb_print("  N bri / bri?    master brightness cap 0..255\r\n");
    usb_print("  N npx / npx?    active pixels/strip (partial strips)\r\n");
    usb_print("  s i r g b ws-px / s r g b ws-fill / r g b ws-fill-all\r\n");
    usb_print("  s ws-show / ws-show-all / s ws-rand-fill\r\n");
}

static void register_words(void) {
    forth_register_word("ws-px",        w_ws_px);
    forth_register_word("ws-fill",      w_ws_fill);
    forth_register_word("ws-fill-all",  w_ws_fill_all);
    forth_register_word("ws-rand-fill", w_ws_rand_fill);
    forth_register_word("ws-show",      w_ws_show);
    forth_register_word("ws-show-all",  w_ws_show_all);
    forth_register_word("ws-clear",     w_ws_clear);
    forth_register_word("bri",          w_bri);
    forth_register_word("bri?",         w_bri_q);
    forth_register_word("npx",          w_npx);
    forth_register_word("npx?",         w_npx_q);
    forth_register_word("rnd",          w_rnd);
    forth_register_word("nap",          w_nap);
    forth_register_word("strips",       w_strips);
}

/* High-level self-test + pattern words, defined in Forth and loaded at boot.
 * Editable live at the REPL — redefine any of these to tweak a test. */
static const char *FORTH_BUNDLE =
    "4 constant NSTRIP\n"
    /* one white pixel marching down a strip, clearing behind it */
    /* low-power bring-up: pixel 0 of each strip, very dim, one at a time.
     * Safe to run on USB power while diagnosing supply/brownout. */
    /* Patterns use full-scale colors; the `bri` cap is the single
     * brightness/current dial (every channel is scaled by it). */
    ": smoke ( -- )\n"
    "  NSTRIP 0 do\n"
    "    i 0 255 255 255 ws-px  i ws-show  400 nap\n"
    "    i 0 0 0 0 ws-px  i ws-show\n"
    "  loop\n"
    "  .\" smoke ok\" cr ;\n"
    /* light first 3 px of one strip green, to identify its wiring */
    ": id ( strip -- )\n"
    "  dup 0 0 255 0 ws-px  dup 1 0 255 0 ws-px\n"
    "  dup 2 0 255 0 ws-px  ws-show ;\n"
    ": walk1 ( strip -- )\n"
    "  dup 0 0 0 ws-fill\n"
    "  npx? 0 do\n"
    "    dup i 255 255 255 ws-px  dup ws-show\n"
    "    35 nap\n"
    "    dup i 0 0 0 ws-px\n"
    "  loop\n"
    "  dup ws-show drop ;\n"
    ": selftest-walk ( -- )\n"
    "  NSTRIP 0 do i walk1 loop\n"
    "  .\" walk done\" cr ;\n"
    ": selftest-allon ( -- )\n"
    "  255 255 255 ws-fill-all  ws-show-all\n"
    "  .\" all strips white -- every LED should be lit\" cr\n"
    "  3000 nap  ws-clear ;\n"
    ": selftest-bars ( -- )\n"
    "  255 0 0 ws-fill-all ws-show-all .\" RED\"   cr 1500 nap\n"
    "  0 255 0 ws-fill-all ws-show-all .\" GREEN\" cr 1500 nap\n"
    "  0 0 255 ws-fill-all ws-show-all .\" BLUE\"  cr 1500 nap\n"
    "  ws-clear  .\" bars done -- R/G/B must match labels\" cr ;\n"
    ": selftest ( -- )\n"
    "  .\" === SELF TEST ===\" cr\n"
    "  selftest-bars  selftest-allon  selftest-walk\n"
    "  .\" === SELF TEST COMPLETE ===\" cr ;\n"
    /* one frame of the ported campsynthesis random-color loop.
     * Strip 3 (orig strip4) keeps its ~2.4 s dwell before showing. */
    ": randframe ( -- )\n"
    "  0 ws-rand-fill 0 ws-show\n"
    "  1 ws-rand-fill 1 ws-show\n"
    "  2 ws-rand-fill 2 ws-show\n"
    "  3 ws-rand-fill 2400 nap 3 ws-show\n"
    "  1000 nap ;\n"
    ": randrun ( frames -- )  0 do randframe loop  ws-clear ;\n";

/* ---- Main ---- */

void app_main(void) {
    setup_usb_serial();
    vTaskDelay(pdMS_TO_TICKS(500));

    usb_print("\r\n\r\n");
    usb_print("============================================\r\n");
    usb_printf("  XIAO ESP32C3 IOT Lighting E4TH v%s\r\n", ESPIDFORTH_VERSION_STRING);
    usb_printf("  Build: %s %s\r\n", ESPIDFORTH_BUILD_DATE, ESPIDFORTH_BUILD_TIME);
    usb_print("  Grove WS2813 (60 px) via RMT + ESPIDFORTH\r\n");
    usb_print("============================================\r\n");

    ws_gpio_init();
    usb_print("Strip 0: Grove D0 (GPIO2 DIN + GPIO3 BIN, RMT). Blanked.\r\n");

    forth_init(FORTH_HEAP_SIZE);
    register_words();
    if (forth_eval(FORTH_BUNDLE) != 0)
        usb_print("[warn] Forth bundle load reported an error\r\n");
    usb_print("Forth engine initialized.\r\n");
    usb_printf("Free heap: %lu bytes\r\n\r\n",
        (unsigned long)esp_get_free_heap_size());

    w_strips();
    usb_printf("\r\n240 WS2812 need an external 5V supply (common ground).\r\n");
    usb_printf("Patterns are full-scale; 'bri' (now %d/255) is the one\r\n",
               (int)s_bri_cap);
    usb_print("brightness+current dial. USB-safe combo: 30 npx  24 bri\r\n");
    usb_print("then 'smoke' / 'selftest'. Raise 'bri' only on a real PSU.\r\n\r\n");

    forth_repl(uart_getchar, uart_putchar);
    forth_deinit();
}
