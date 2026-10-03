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
#include "magnet_ota.h"
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

/* ---- RX: LDMA → ring buffer → link_getc ----
 * Received by DMA, not per-byte interrupts: the USART FIFO is 2 bytes and the
 * CPU stalls longer than that at 115200 during flash erases and BLE radio
 * work (see boards/xiao_mg24.overlay). Two 384-byte buffers ping-pong — each
 * holds ~33 ms of line, so a stall up to ~66 ms loses nothing — and a 1 ms
 * idle timeout hands partial buffers over promptly. */
RING_BUF_DECLARE(s_rx_ring, 1024);
static K_SEM_DEFINE(s_rx_sem, 0, 1);
static uint8_t s_dma_buf[2][384];
static uint8_t s_dma_next;
/* The UART has no flow control: if the dispatcher falls > 1 KB behind the
 * wire, bytes are lost. Counted here, reported by link_getc as a !WARN so a
 * host knows its data was dropped (hosts should pace on replies; see
 * tools/ota_push.py). */
static atomic_t s_rx_dropped;

static void uart_cb(const struct device *dev, struct uart_event *evt, void *ud) {
    ARG_UNUSED(ud);
    switch (evt->type) {
    case UART_RX_RDY:
    {
        uint32_t put = ring_buf_put(&s_rx_ring, evt->data.rx.buf + evt->data.rx.offset,
                                    evt->data.rx.len);
        if (put < evt->data.rx.len) atomic_add(&s_rx_dropped, evt->data.rx.len - put);
        k_sem_give(&s_rx_sem);
    }
        break;
    case UART_RX_BUF_REQUEST:
        uart_rx_buf_rsp(dev, s_dma_buf[s_dma_next], sizeof(s_dma_buf[0]));
        s_dma_next ^= 1;
        break;
    case UART_RX_DISABLED:                  /* after an error/stop: start over */
        s_dma_next = 1;
        uart_rx_enable(dev, s_dma_buf[0], sizeof(s_dma_buf[0]), 1000);
        break;
    default:
        break;
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
    static atomic_val_t reported;
    atomic_val_t dropped = atomic_get(&s_rx_dropped);
    if (dropped != reported) {                   /* dispatcher context: may emit */
        mn_emit_event("!WARN link-rx-overrun dropped=%ld (host: wait for each reply)",
                      (long)(dropped - reported));
        reported = dropped;
    }
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
    uart_callback_set(s_uart, uart_cb, NULL);
    s_dma_next = 1;
    uart_rx_enable(s_uart, s_dma_buf[0], sizeof(s_dma_buf[0]), 1000);   /* 1 ms idle */

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
        if (mn_ota_bundles_suspended()) {     /* CLEARS_BUNDLES image on trial (§7.4) */
            raw_print("# ota: CLEARS_BUNDLES image on trial — saved bundles held back\r\n");
        } else {
            if (craw_role_bundle_apply_saved(caps, ncaps) > 0)
                raw_print("# autorun: persisted role bundle(s) re-applied\r\n");
            int found, failed;               /* OTA health: a failed re-apply blocks confirm */
            craw_role_bundle_boot_stats(&found, &failed);
            mn_boot_bundles_set(found, failed);
        }
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
        mn_ota_sink_init();          /* mesh OTA: "mnpkg:1" packages -> slot1 */
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
