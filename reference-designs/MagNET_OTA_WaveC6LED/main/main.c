/*
 * MagNET OTA client — Waveshare ESP32-C6-LCD-1.47 ("WaveC6LED")
 *
 * D0: board bring-up. Forth REPL over USB-serial-JTAG, nothing else yet.
 * Derived from ESPIDFORTH's src/main.c.
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

    int rc = forth_init(FORTH_HEAP_SIZE);
    if (rc != 0) {
        usb_printf("forth_init failed (rc=%d)\r\n", rc);
        return;
    }
    usb_printf("forth ready, %lu bytes free\r\n",
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    forth_repl(repl_getchar, repl_putchar);
    forth_deinit();
}
