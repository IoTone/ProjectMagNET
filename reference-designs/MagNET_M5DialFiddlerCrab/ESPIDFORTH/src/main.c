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

static void print_memory_stats(const char *label) {
    printf("\n=== Memory Stats: %s ===\n", label);
    printf("  Free heap (internal): %lu bytes\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("  Largest free block:   %lu bytes\n",
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("  Min free ever:        %lu bytes\n",
           (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
#if CONFIG_SPIRAM
    printf("  Free PSRAM:           %lu bytes\n",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("  Largest PSRAM block:  %lu bytes\n",
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#endif
    printf("===========================\n\n");
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

static void forth_repl_task(void *arg) {

    /* Run the Forth REPL (blocking) */
    forth_repl(uart_getchar, uart_putchar);

    forth_deinit();
    vTaskDelete(NULL);
}

void app_main(void) {
    /* Set up USB-serial-JTAG driver for raw char I/O */
    setup_usb_serial();

    printf("\n\n");
    printf("============================================\n");
    printf("  ESPIDFORTH v%s\n", ESPIDFORTH_VERSION_STRING);
    printf("  Build: %s %s\n", ESPIDFORTH_BUILD_DATE, ESPIDFORTH_BUILD_TIME);
    printf("  Phase 2: MagNET Hive AI Prototype\n");
    printf("============================================\n");
    fflush(stdout);

    print_memory_stats("Before Forth init");

    /* Determine heap size: use more on PSRAM-equipped targets */
    int heap_size = FORTH_HEAP_SIZE;
#if CONFIG_SPIRAM
    heap_size = 512 * 1024;  /* 512 KB when PSRAM available */
    ESP_LOGI(TAG, "PSRAM detected, using %d KB Forth heap", heap_size / 1024);
#else
    ESP_LOGI(TAG, "No PSRAM, using %d KB Forth heap", heap_size / 1024);
#endif

    ESP_LOGI(TAG, "Initializing Forth engine...");
    int rc = forth_init(heap_size);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initialize Forth engine (rc=%d)", rc);
        return;
    }
    ESP_LOGI(TAG, "Forth engine initialized.");

    print_memory_stats("After Forth init");

    /* Launch REPL on a task with adequate stack */
    xTaskCreate(forth_repl_task, "forth_repl", 8192, NULL, 5, NULL);
}
