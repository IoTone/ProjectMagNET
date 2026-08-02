/*
 * MagNET OTA client — Waveshare ESP32-C6-LCD-1.47 ("WaveC6LED")
 *
 * D0: board bring-up. Forth REPL over USB-serial-JTAG, ST7789 status screen,
 * RGB LED. Derived from ESPIDFORTH's src/main.c.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "forth_core.h"
#include "forth_version.h"
#include "magnet_ui.h"

/* 100 KB dictionary. No PSRAM on this board, and 512 KB of HP SRAM has to also
 * hold WiFi + TLS from D1 onward, so this stays modest until measured. */
#define FORTH_HEAP_SIZE (100 * 1024)

static void usb_print(const char *s) {
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(500));
}

static void usb_printf(const char *fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    usb_print(buf);
}

static int repl_getchar(void) {
    uint8_t c;
    int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
    return (n <= 0) ? -1 : c;
}

static void repl_putchar(int c) {
    uint8_t ch = (uint8_t)c;
    usb_serial_jtag_write_bytes(&ch, 1, pdMS_TO_TICKS(100));
}

/* The status screen. Everything the device knows about itself, in the order a
 * human standing in front of it wants to read it. Redrawn wholesale rather than
 * diffed — at 172x320 with no framebuffer, a full repaint is cheap and there is
 * no partial-update state to get wrong. */
static void draw_status(const char *state, uint16_t state_colour,
                        unsigned long heap_free) {
    char line[32];
    ui_clear(UI_BG);

    ui_fill(0, 0, UI_W, 3, UI_CYAN);
    ui_text(6, 12, "ROBOTARME", UI_CYAN, UI_BG, 2);
    ui_text(6, 32, "OTA CLIENT", UI_DIM, UI_BG, 1);

    ui_fill(6, 48, UI_W - 12, 1, UI_DIM);

    ui_text(6, 60, "DEVICE", UI_DIM, UI_BG, 1);
    ui_text(6, 74, "WAVEC6LED", UI_WHITE, UI_BG, 2);

    ui_text(6, 104, "FIRMWARE", UI_DIM, UI_BG, 1);
    snprintf(line, sizeof(line), "E4TH %s", ESPIDFORTH_VERSION_STRING);
    ui_text(6, 118, line, UI_AMBER, UI_BG, 2);

    ui_text(6, 148, "STATE", UI_DIM, UI_BG, 1);
    ui_text(6, 162, state, state_colour, UI_BG, 2);

    ui_text(6, 200, "FREE RAM", UI_DIM, UI_BG, 1);
    snprintf(line, sizeof(line), "%lu KB", heap_free / 1024);
    ui_text(6, 214, line, UI_WHITE, UI_BG, 2);

    /* No server yet: say so, rather than leaving a blank a reader has to
     * interpret. D2 replaces this with the real check-in state. */
    ui_text(6, 252, "SERVER", UI_DIM, UI_BG, 1);
    ui_text(6, 266, "NOT SET", UI_DIM, UI_BG, 1);

    ui_fill(0, UI_H - 3, UI_W, 3, UI_CYAN);
}

void app_main(void) {
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    usb_serial_jtag_driver_install(&cfg);

    /* Let the USB host enumerate before we start talking into the void. */
    vTaskDelay(pdMS_TO_TICKS(500));

    usb_print("\r\n\r\n============================================\r\n");
    usb_print("  MagNET OTA client - WaveC6LED\r\n");
    usb_printf("  ESPIDFORTH v%s\r\n", ESPIDFORTH_VERSION_STRING);
    usb_printf("  Build: %s %s\r\n", ESPIDFORTH_BUILD_DATE, ESPIDFORTH_BUILD_TIME);
    usb_print("============================================\r\n");
    usb_printf("free heap: %lu bytes\r\n",
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (ui_init() == ESP_OK) {
        usb_print("display: ST7789 172x320 up\r\n");
        draw_status("BOOTING", UI_AMBER,
                    heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    } else {
        usb_print("display: FAILED\r\n");
    }
    if (led_init() == ESP_OK) {
        led_rgb(40, 24, 0);                 /* amber: booting */
        usb_print("led: WS2812 on GPIO8 up\r\n");
    } else {
        usb_print("led: FAILED\r\n");
    }

    int rc = forth_init(FORTH_HEAP_SIZE);
    if (rc != 0) {
        usb_printf("forth_init failed (rc=%d)\r\n", rc);
        return;
    }
    unsigned long freeb = (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    usb_printf("forth ready, %lu bytes free\r\n", freeb);

    /* IDLE, not READY: nothing is provisioned yet and the device is not doing
     * anything on anyone's behalf. Overstating readiness on a status display is
     * how a demo goes wrong in front of people. */
    draw_status("IDLE", UI_GREEN, freeb);
    led_rgb(0, 24, 8);                      /* dim green: idle, alive */

    forth_repl(repl_getchar, repl_putchar);
    forth_deinit();
}
