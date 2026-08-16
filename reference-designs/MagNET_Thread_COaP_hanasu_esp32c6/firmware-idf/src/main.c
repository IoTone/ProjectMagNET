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
#include "nvs_flash.h"
#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"

#include "forth_core.h"
#include "craw_role_bundle.h"
#include "magnet.h"
#include "magnet_led.h"

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
/* Serial TX must NEVER stall the node. With no host reading USB-CDC the
 * driver's TX ring fills and a blocking write waits out its full timeout —
 * at the old 100 ms/char a 60-char response cost ~6 s, and because every
 * line (BLE responses included) funnels through the one TX writer, a
 * phone-only companion node went progressively deaf: each BLE command's
 * dispatch queued behind the previous response's serial stall until the
 * 6-deep BLE RX queue overflowed and dropped verbs outright (found by the
 * 2026-08-02 hardware pass; macOS repro: 0 timeouts with a serial reader
 * attached, 7/9 without). Policy: first full-buffer write waits one short
 * grace period; while stalled, drop serial output at 0 timeout (BLE mirror
 * already went out) and recover the moment a write succeeds again. */
static void link_putc(int c) {
    static bool stalled = false;
    uint8_t ch = (uint8_t)c;
    int n = usb_serial_jtag_write_bytes(&ch, 1,
                                        stalled ? 0 : pdMS_TO_TICKS(20));
    stalled = (n <= 0);
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

    /* 3. NVS (identity/name/counter storage), then MagNET core + Forth vocab.
     *    §12.5 order: storage before core, radios last. nvs_flash_init is
     *    idempotent — magnet_ot's own guard call becomes a no-op. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    mn_core_init();
    mn_register_forth_vocab();
    /* E-E: user automation script runs once, after the mn-* words exist and
     * before the radios come up (so hooks are armed when traffic starts). */
    /* H6: persisted SIGNED bundles re-apply first (verified path), then the
     * unsigned dev script — so a script can build on bundle-defined words,
     * and fleet behavior never depends on the unsigned channel. */
    craw_role_bundle_init();
    {
        int ncaps = 0;
        const char **caps = mn_bundle_caps(&ncaps);
        int applied = craw_role_bundle_apply_saved(caps, ncaps);
        if (applied > 0) raw_print("# autorun: persisted role bundle(s) re-applied\r\n");
    }
    if (mn_script_run() == 0) raw_print("# autorun: boot script executed\r\n");

    /* 4. start the dual-mode host link (HCP default). This also wires the
     *    serialized TX writer and forth_set_io, so events/responses are safe. */
    mn_link_start(link_getc, link_putc);
    mn_set_state(MN_BOOTING);
    /* id is provisional here — the EUI-64-derived device_id lands during radio
     * bringup (ot_configure); WHOAMI/STATUS report the real one once up. */
    mn_emit_event("!READY proto=2.1 fw=" MN_FW_VERSION " id=%02x%02x%02x%02x name=%s state=BOOTING",
                  mn_device_id()[0], mn_device_id()[1], mn_device_id()[2], mn_device_id()[3],
                  mn_name_get());
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
#if MN_BLE_RESIDENT
    /* Companion build (magnet_app SCOPE §1 Option A, decided 2026-08-02):
     * BLE stays up for the life of the node, provisioned or not, so the phone
     * app can attach to THIS node as its live window into the mesh. This
     * deliberately re-opens the bonding window on every boot — acceptable for
     * the one bench/diagnostic node, which is why deployed nodes keep the
     * provisioning-only build below. Privileged verbs still require a bonded
     * link (per-verb E_NOT_BONDED enforcement). */
    if (mn_ble_start() == 0) raw_print("# ble: resident (companion node)\r\n");
#elif MN_ENABLE_BLE
    /* Provisioning-only BLE (§11.2.1 / §12.9 Q3): advertise so a phone can
     * push a credential with no cable. mn_channel_set() tears the stack down
     * the moment a channel lands, returning the radio + RAM to Thread. */
    /* Only advertise when the node is still UNPROVISIONED (default channel).
     * A node that already holds a private channel must not re-open a bonding
     * window on every power cycle — that would let anyone re-provision it by
     * cycling the power. Factory-reset (clear NVS) to provision again. */
    if (mn_channel_is_default()) {
        if (mn_ble_start() == 0) raw_print("# ble: provisioning window open\r\n");
    } else {
        raw_print("# ble: skipped (already provisioned)\r\n");
    }
#endif
    mn_set_state(MN_CONFIGURING);
    mn_openthread_start();

#if MN_ENABLE_LED
    /* LAST on purpose: earlier bring-up (radio/BLE/forth) must not get a
     * chance to reclaim the LED pins after we matrix them to RMT. */
    {
        int lrc = mn_led_init();
        char lbuf[48];
        snprintf(lbuf, sizeof lbuf, "# led: init rc=%d\r\n", lrc);
        raw_print(lbuf);
    }
#endif

    report_heap("after openthread_start");

    /* dispatcher + OT run in their own tasks; nothing left to do here */
    vTaskDelay(portMAX_DELAY);
}
