/*
 * ESPIDFORTH - ESP32FORTH Port to ESP-IDF
 * Phase 2 of MagNET Hive AI prototype
 *
 * Boots Forth interpreter and provides UART REPL.
 * Reports memory stats at startup.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "forth_core.h"
#include "forth_version.h"

static const char *TAG = "espidforth";

/* Default Forth dictionary heap size (100 KB) */
#define FORTH_HEAP_SIZE (100 * 1024)

/* Write a string via the USB-serial-JTAG driver */
static void usb_print(const char *s) {
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(500));
}

/* snprintf + usb_print helper */
static void usb_printf(const char *fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    usb_print(buf);
}

static void print_memory_stats(const char *label) {
    usb_printf("\r\n=== Memory Stats: %s ===\r\n", label);
    usb_printf("  Free heap (internal): %lu bytes\r\n",
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    usb_printf("  Largest free block:   %lu bytes\r\n",
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    usb_printf("  Min free ever:        %lu bytes\r\n",
               (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
#if CONFIG_SPIRAM
    usb_printf("  Free PSRAM:           %lu bytes\r\n",
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    usb_printf("  Largest PSRAM block:  %lu bytes\r\n",
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#endif
    usb_print("===========================\r\n\r\n");
}

/* Direct USB-serial-JTAG character I/O — bypasses VFS line buffering */
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


void app_main(void) {
    /* Set up USB-serial-JTAG driver for raw char I/O */
    setup_usb_serial();

    /* Small delay to let USB host enumerate before sending */
    vTaskDelay(pdMS_TO_TICKS(500));

    usb_print("\r\n\r\n");
    usb_print("============================================\r\n");
    usb_printf("  ESPIDFORTH v%s\r\n", ESPIDFORTH_VERSION_STRING);
    usb_printf("  Build: %s %s\r\n", ESPIDFORTH_BUILD_DATE, ESPIDFORTH_BUILD_TIME);
    usb_print("  Phase 2: MagNET Hive AI Prototype\r\n");
    usb_print("============================================\r\n");

    print_memory_stats("Before Forth init");

    /* Determine heap size: use more on PSRAM-equipped targets */
    int heap_size = FORTH_HEAP_SIZE;
#if CONFIG_SPIRAM
    heap_size = 512 * 1024;  /* 512 KB when PSRAM available */
    usb_printf("PSRAM detected, using %d KB Forth heap\r\n", heap_size / 1024);
#else
    usb_printf("No PSRAM, using %d KB Forth heap\r\n", heap_size / 1024);
#endif

    usb_print("Initializing Forth engine...\r\n");
    int rc = forth_init(heap_size);
    if (rc != 0) {
        usb_printf("Failed to initialize Forth engine (rc=%d)\r\n", rc);
        return;
    }
    usb_print("Forth engine initialized.\r\n");

    print_memory_stats("After Forth init");

    /* Run REPL directly in app_main (blocks forever) */
    forth_repl(uart_getchar, uart_putchar);

    /* Only reached if user types 'bye' */
    forth_deinit();
}
