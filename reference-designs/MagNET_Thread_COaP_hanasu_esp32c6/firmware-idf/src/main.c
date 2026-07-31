/*
 * main.c — MagNET Hanasu node, E-Phase A coexistence spike.
 *
 * Proves ESPIDFORTH + esp_openthread + mbedTLS run together on a no-PSRAM C6,
 * and brings the pieces up in the load-bearing order from design proposal §12.5:
 *
 *   transport → forth_init (claim heap first) → vocab → core → link → RADIOS LAST
 *
 * Console is the C6 native USB-serial-JTAG (same path ESPIDFORTH uses on C3/C6).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"

#include "forth_core.h"
#include "magnet.h"

/* C6 has no PSRAM and must also host OpenThread + mbedTLS, so the Forth
 * dictionary heap is cut from ESPIDFORTH's default 100 KB (design proposal
 * §12.5). 64 KB is the starting point — tune from the `mem` low-water mark. */
#define FORTH_HEAP_SIZE (64 * 1024)

/* ---- raw transport char I/O ---- */
static int link_getc(void) {
    uint8_t c;
    int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
    return (n > 0) ? c : -1;
}
static void link_putc(int c) {
    uint8_t ch = (uint8_t)c;
    usb_serial_jtag_write_bytes(&ch, 1, pdMS_TO_TICKS(100));
}
static void raw_print(const char *s) {
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(500));
}

static void report_heap(const char *label) {
    char buf[128];
    snprintf(buf, sizeof(buf),
        "# heap[%s] free=%u largest=%u min=%u\r\n", label,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    raw_print(buf);
}

void app_main(void) {
    /* 1. transport up first so we can talk even if everything else stalls */
    usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 512, .rx_buffer_size = 512 };
    usb_serial_jtag_driver_install(&cfg);
    vTaskDelay(pdMS_TO_TICKS(500));            /* let USB host enumerate */

    raw_print("\r\n# MagNET Hanasu — E-Phase B (plaintext mesh chat)\r\n");
    report_heap("boot");

    /* 2. claim the Forth dictionary heap while RAM is unfragmented */
    if (forth_init(FORTH_HEAP_SIZE) != 0) {
        raw_print("-ERR E_INTERNAL forth_init failed\r\n");
        return;
    }
    report_heap("after forth_init");

    /* 3. MagNET core + Forth FFI vocabulary */
    mn_core_init();
    mn_register_forth_vocab();

    /* 4. start the dual-mode host link (HCP default). This also wires the
     *    serialized TX writer and forth_set_io, so events/responses are safe. */
    mn_link_start(link_getc, link_putc);
    mn_set_state(MN_BOOTING);
    /* id is provisional here — the EUI-64-derived device_id lands during radio
     * bringup (ot_configure); WHOAMI/STATUS report the real one once up. */
    mn_emit_event("!READY proto=2.1 fw=0.2.0-eb id=%02x%02x%02x%02x name=- state=BOOTING",
                  mn_device_id()[0], mn_device_id()[1], mn_device_id()[2], mn_device_id()[3]);
    mn_emit_event("# type CAPS, or HELP. FORTH drops to the engine.");

    /* 5. bring up radios LAST — heap is committed by now */
#if MN_XIAO_RF_SWITCH
    /* XIAO ESP32C6 routes the radio through an RF switch: GPIO3 low enables
     * it, GPIO14 low selects the built-in ceramic antenna (high = ext. UFL).
     * Must be set before the radio starts or the node is antenna-less. */
    gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_3, 0);
    gpio_set_direction(GPIO_NUM_14, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_14, 0);
    mn_emit_event("# xiao rf-switch: enabled, internal antenna");
#endif
    mn_set_state(MN_CONFIGURING);
    mn_openthread_start();

    report_heap("after openthread_start");

    /* dispatcher + OT run in their own tasks; nothing left to do here */
    vTaskDelay(portMAX_DELAY);
}
