/*
 * main.c — MagNET Hanasu node on Zephyr (XIAO MG24), phase Z-B.
 *
 * Same bring-up order as firmware-idf/src/main.c (§12.5), which is
 * load-bearing: transport → forth_init (claim heap first) → storage → core +
 * vocab → link → RADIOS LAST.
 *
 * Host link: USART0 at 115200, which the XIAO's SAMD11 bridges to USB CDC.
 * This UART is ALSO Zephyr's default console, so prj.conf turns the console,
 * shell and logging off — any stray line here breaks HCP framing (§11.3).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "forth_core.h"
#include "craw_role_bundle.h"
#include "magnet.h"

/* Half the C6's 64 KB. The Forth heap only backs allot/create data —
 * colon definitions (incl. installed role bundles) live in the engine's
 * static dictionary/code arrays — and SYSINFO showed forth-heap used=0 with
 * two bundles installed, while a BUNDLE COMMIT (6 KB accumulator + cJSON
 * tree + Ed25519) drove the shared malloc arena down to 1.4 KB free. */
#ifndef MN_FORTH_HEAP_KB
#define MN_FORTH_HEAP_KB 32        /* BLE build: 16 (CMakeLists.txt) */
#endif
#define FORTH_HEAP_SIZE (MN_FORTH_HEAP_KB * 1024)

static const struct device *const s_uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* ---- RX: IRQ → ring buffer → link_getc ---- */
RING_BUF_DECLARE(s_rx_ring, 512);
static K_SEM_DEFINE(s_rx_sem, 0, 1);

static void uart_isr(const struct device *dev, void *ud) {
    ARG_UNUSED(ud);
    uart_irq_update(dev);
    while (uart_irq_rx_ready(dev)) {
        uint8_t buf[32];
        int n = uart_fifo_read(dev, buf, sizeof(buf));
        if (n > 0) {
            ring_buf_put(&s_rx_ring, buf, n);  /* full ring drops: host re-sends */
            k_sem_give(&s_rx_sem);
        } else {
            break;
        }
    }
}

/* One char, or -1 after ~10 ms — the contract the IDF getc has.
 *
 * NUL bytes are dropped: the SAMD11 bridge injects a 0x00 into the UART when
 * the host opens the port, and a line that starts with NUL parses as empty
 * (the dispatcher answers "-ERR E_SYNTAX empty" and loses the whole command).
 * HCP is text, so a NUL is never meaningful. */
static int link_getc(void) {
    uint8_t c;
    for (;;) {
        if (ring_buf_get(&s_rx_ring, &c, 1) != 1) {
            k_sem_take(&s_rx_sem, K_MSEC(10));
            if (ring_buf_get(&s_rx_ring, &c, 1) != 1) return -1;
        }
        if (c != 0) return c;
    }
}

/* Polled TX. Unlike the C6's on-die USB, the SAMD11 bridge always drains the
 * UART whether or not a host has the port open, so this cannot stall the way
 * link_putc on IDF could (which is why that one drops on a full buffer). */
static void link_putc(int c) { uart_poll_out(s_uart, (unsigned char)c); }

static void raw_print(const char *s) { while (*s) link_putc(*s++); }

static void report_heap(const char *label) {
    char buf[128];
    snprintf(buf, sizeof(buf), "# heap[%s] free=%u min=%u\r\n", label,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    raw_print(buf);
}

int main(void) {
    /* 1. transport up first so we can talk even if everything else stalls */
    if (!device_is_ready(s_uart)) return 0;
    uart_irq_callback_user_data_set(s_uart, uart_isr, NULL);
    uart_irq_rx_enable(s_uart);

    raw_print("\r\n# MagNET Hanasu — Zephyr/MG24 (Z-B)\r\n");
    report_heap("boot");

    /* 2. claim the Forth dictionary heap while RAM is unfragmented */
    if (forth_init(FORTH_HEAP_SIZE) != 0) {
        raw_print("-ERR E_INTERNAL forth_init failed\r\n");
        return 0;
    }
    report_heap("after forth_init");

    /* 3. storage, then MagNET core + Forth vocab */
    if (nvs_flash_init() != ESP_OK) raw_print("# WARN settings init failed\r\n");
    {   /* power-on crypto KATs: PSA port == C6 mbedTLS */
        extern void mn_crypto_psa_kat(char *out, size_t cap);
        char kat[200];
        mn_crypto_psa_kat(kat, sizeof kat);
        raw_print(kat); raw_print("\r\n");
    }
    mn_core_init();
    mn_register_forth_vocab();
    craw_role_bundle_init();
    {
        int ncaps = 0;
        const char **caps = mn_bundle_caps(&ncaps);
        if (craw_role_bundle_apply_saved(caps, ncaps) > 0)
            raw_print("# autorun: persisted role bundle(s) re-applied\r\n");
    }
    if (mn_script_run() == 0) raw_print("# autorun: boot script executed\r\n");

    /* 4. dual-mode host link (HCP default) + serialized TX writer */
    mn_link_start(link_getc, link_putc);
    mn_set_state(MN_BOOTING);
    mn_emit_event("!READY proto=2.1 fw=" MN_FW_VERSION " id=%02x%02x%02x%02x name=%s state=BOOTING",
                  mn_device_id()[0], mn_device_id()[1], mn_device_id()[2], mn_device_id()[3],
                  mn_name_get());
    mn_emit_event("# type CAPS, or HELP. FORTH drops to the engine.");
    {   /* Z-F: report the image and arm the confirm-when-healthy policy */
        extern void mn_ota_boot(void);
        extern void mn_ota_sink_init(void);
        mn_ota_boot();
        mn_ota_sink_init();          /* mesh OTA: "ota:" transfers -> slot1 */
    }

#if MN_BLE_RESIDENT
    /* companion node: attachable for life (see firmware-idf/src/main.c) */
    if (mn_ble_start() == 0) raw_print("# ble: resident (companion node)\r\n");
#elif MN_ENABLE_BLE
    /* provisioning-only: advertise only while still on the default channel,
     * so a power cycle never re-opens a bonding window on a deployed node */
    if (mn_channel_is_default()) {
        if (mn_ble_start() == 0) raw_print("# ble: provisioning window open\r\n");
    } else {
        raw_print("# ble: skipped (already provisioned)\r\n");
    }
#endif

    /* 5. radios LAST. The XIAO MG24's RF switch (PB5 enable, PB4 antenna
     * select) is set by gpio-hogs in the board DTS — nothing to do here,
     * unlike the XIAO C6. */
    mn_set_state(MN_CONFIGURING);
    mn_openthread_start();

    report_heap("after openthread_start");
    return 0;                          /* link, pump and OT run in their own threads */
}
