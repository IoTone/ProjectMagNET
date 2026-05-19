/*
 * XIAO ESP32C3 IOT Lighting — E4TH (Phase 1: port + self-test)
 *
 * Port of the campsynthesis 4-strip random-color NeoPixel sketch to the
 * ESPIDFORTH ("E4TH") runtime. Four WS2812/NeoPixel strips of 60 LEDs each,
 * driven by a software bit-bang on GPIO 0, 1, 2, 5 (Xiao ESP32C3 in a
 * Grove Shield for XIAO).
 *
 * The ESP32-C3 has only 2 RMT TX channels / 1 SPI host, so it cannot drive
 * 4 independent WS2812 strips with the hardware led_strip driver — hence the
 * cycle-counted bit-bang here, exposed to Forth as FFI words.
 *
 * Logical strip index 0..3 maps to the original sketch's strip1..4:
 *   strip 0 -> GPIO 0   (orig strip1)
 *   strip 1 -> GPIO 5   (orig strip2)
 *   strip 2 -> GPIO 1   (orig strip3)
 *   strip 3 -> GPIO 2   (orig strip4)
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
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "soc/gpio_reg.h"
#include "forth_core.h"
#include "forth_version.h"

#define STRIP_COUNT 4
#define NUM_PIXELS  60
#define BYTES_PER   (NUM_PIXELS * 3)
#define FORTH_HEAP_SIZE (64 * 1024)

/* Logical strip -> GPIO. Order preserves the original sketch's strip1..4. */
static const int STRIP_GPIO[STRIP_COUNT] = { 0, 5, 1, 2 };

/* Per-strip framebuffer, WS2812 wire order = G,R,B. */
static uint8_t s_fb[STRIP_COUNT][BYTES_PER];

/* WS2812 @ 800 kHz, timed in CPU cycles off the RISC-V cycle counter. */
#define F_CPU_HZ   ((uint32_t)(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) * 1000000UL)
#define WS_T1H_CY  (F_CPU_HZ / 1250000UL)  /* ~0.80 us high  -> bit '1' */
#define WS_T0H_CY  (F_CPU_HZ / 2500000UL)  /* ~0.40 us high  -> bit '0' */
#define WS_BIT_CY  (F_CPU_HZ /  800000UL)  /* ~1.25 us total per bit    */

static portMUX_TYPE s_ws_mux = portMUX_INITIALIZER_UNLOCKED;

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

/* ---- WS2812 bit-bang driver ---- */

static void ws_gpio_init(void) {
    uint64_t mask = 0;
    for (int s = 0; s < STRIP_COUNT; s++) mask |= (1ULL << STRIP_GPIO[s]);
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    for (int s = 0; s < STRIP_COUNT; s++) gpio_set_level(STRIP_GPIO[s], 0);
    memset(s_fb, 0, sizeof(s_fb));
}

static inline int strip_ok(int s) { return s >= 0 && s < STRIP_COUNT; }

static void ws_set_pixel(int s, int i, uint8_t r, uint8_t g, uint8_t b) {
    if (!strip_ok(s) || i < 0 || i >= NUM_PIXELS) return;
    uint8_t *p = &s_fb[s][i * 3];
    p[0] = g; p[1] = r; p[2] = b;   /* GRB on the wire */
}

static void ws_fill(int s, uint8_t r, uint8_t g, uint8_t b) {
    if (!strip_ok(s)) return;
    for (int i = 0; i < NUM_PIXELS; i++) ws_set_pixel(s, i, r, g, b);
}

static void ws_rand_fill(int s) {
    if (!strip_ok(s)) return;
    for (int i = 0; i < NUM_PIXELS; i++) {
        uint32_t v = esp_random();
        ws_set_pixel(s, i, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    }
}

/* Blast one strip's framebuffer out the wire. Interrupts are disabled for
 * the duration (~7 ms for 60 px) so the sub-microsecond pulse widths hold;
 * the critical section is released between strips so the FreeRTOS tick can
 * make progress. */
static IRAM_ATTR void ws_blast(int gpio, const uint8_t *buf, size_t n) {
    const uint32_t bit = 1UL << gpio;
    portENTER_CRITICAL(&s_ws_mux);
    uint32_t start = esp_cpu_get_cycle_count();
    for (size_t k = 0; k < n; k++) {
        uint8_t byte = buf[k];
        for (int b = 0; b < 8; b++) {
            uint32_t hi = (byte & 0x80) ? WS_T1H_CY : WS_T0H_CY;
            byte <<= 1;
            while ((esp_cpu_get_cycle_count() - start) < WS_BIT_CY) { }
            REG_WRITE(GPIO_OUT_W1TS_REG, bit);
            start = esp_cpu_get_cycle_count();
            while ((esp_cpu_get_cycle_count() - start) < hi) { }
            REG_WRITE(GPIO_OUT_W1TC_REG, bit);
        }
    }
    portEXIT_CRITICAL(&s_ws_mux);
    esp_rom_delay_us(300);   /* latch / reset */
}

static void ws_show(int s) {
    if (!strip_ok(s)) return;
    ws_blast(STRIP_GPIO[s], s_fb[s], BYTES_PER);
}

static void ws_show_all(void) {
    for (int s = 0; s < STRIP_COUNT; s++) ws_show(s);
}

static void ws_clear_all(void) {
    memset(s_fb, 0, sizeof(s_fb));
    ws_show_all();
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
    usb_print("\r\n4x WS2812 strips, 60 px each (bit-bang):\r\n");
    for (int s = 0; s < STRIP_COUNT; s++)
        usb_printf("  strip %d -> GPIO %d\r\n", s, STRIP_GPIO[s]);
    usb_print("Words:\r\n");
    usb_print("  selftest        bars + all-on + walk\r\n");
    usb_print("  selftest-bars   R / G / B fill (channel order)\r\n");
    usb_print("  selftest-allon  all strips dim white 3 s\r\n");
    usb_print("  selftest-walk   marching pixel per strip\r\n");
    usb_print("  N randrun       N frames of random pattern\r\n");
    usb_print("  ws-clear        blank all strips\r\n");
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
    forth_register_word("rnd",          w_rnd);
    forth_register_word("nap",          w_nap);
    forth_register_word("strips",       w_strips);
}

/* High-level self-test + pattern words, defined in Forth and loaded at boot.
 * Editable live at the REPL — redefine any of these to tweak a test. */
static const char *FORTH_BUNDLE =
    "4 constant NSTRIP\n"
    "60 constant NPX\n"
    /* one white pixel marching down a strip, clearing behind it */
    ": walk1 ( strip -- )\n"
    "  dup 0 0 0 ws-fill\n"
    "  NPX 0 do\n"
    "    dup i 32 32 32 ws-px  dup ws-show\n"
    "    25 nap\n"
    "    dup i 0 0 0 ws-px\n"
    "  loop\n"
    "  dup ws-show drop ;\n"
    ": selftest-walk ( -- )\n"
    "  NSTRIP 0 do i walk1 loop\n"
    "  .\" walk done\" cr ;\n"
    ": selftest-allon ( -- )\n"
    "  24 24 24 ws-fill-all  ws-show-all\n"
    "  .\" all strips dim white -- every LED should be lit\" cr\n"
    "  3000 nap  ws-clear ;\n"
    ": selftest-bars ( -- )\n"
    "  64 0 0 ws-fill-all ws-show-all .\" RED\"   cr 1500 nap\n"
    "  0 64 0 ws-fill-all ws-show-all .\" GREEN\" cr 1500 nap\n"
    "  0 0 64 ws-fill-all ws-show-all .\" BLUE\"  cr 1500 nap\n"
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
    usb_print("  4x WS2812 (60 px) bit-bang via ESPIDFORTH\r\n");
    usb_print("============================================\r\n");

    ws_gpio_init();
    usb_print("Strips: GPIO 0,5,1,2 (logical 0..3). All blanked.\r\n");

    forth_init(FORTH_HEAP_SIZE);
    register_words();
    if (forth_eval(FORTH_BUNDLE) != 0)
        usb_print("[warn] Forth bundle load reported an error\r\n");
    usb_print("Forth engine initialized.\r\n");
    usb_printf("Free heap: %lu bytes\r\n\r\n",
        (unsigned long)esp_get_free_heap_size());

    w_strips();
    usb_print("\r\nType 'selftest' to verify all strips.\r\n\r\n");

    forth_repl(uart_getchar, uart_putchar);
    forth_deinit();
}
