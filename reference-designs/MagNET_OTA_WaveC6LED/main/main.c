/*
 * MagNET OTA client — Waveshare ESP32-C6-LCD-1.47 ("WaveC6LED")
 *
 * D0: Forth REPL over USB-serial-JTAG, ST7789 status screen, RGB LED.
 * D1: WiFi + NVS-backed provisioning; SSID and IP on screen.
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
#include "magnet_ui.h"
#include "magnet_cfg.h"
#include "craw_wifi.h"
#include "console.h"

/* 100 KB dictionary. No PSRAM on this board, and 512 KB of HP SRAM has to also
 * hold WiFi + TLS from D1 onward, so this stays modest until measured. */
#define FORTH_HEAP_SIZE (100 * 1024)

/*
 * usb_serial_jtag_write_bytes() returns the number of bytes it ACCEPTED, which
 * can be fewer than asked for — the TX buffer is 256 bytes and it does not
 * block until the rest drains. Treating it as all-or-nothing silently truncates
 * long output: `show`, whose longest single write is ~60 bytes, worked
 * perfectly while `help` at ~230 bytes vanished completely and looked like a
 * command-dispatch bug.
 *
 * Loop until the string is actually gone. Same lesson as blit() on the display
 * side: an API that reports partial completion has to be driven to completion.
 */
static void usb_print(const char *s) {
    /* CHUNK BELOW THE TX BUFFER. usb_serial_jtag_write_bytes() does not do a
     * partial write when asked for more than its buffer holds (256 bytes here)
     * — it returns 0. So a 291-byte help text vanished ENTIRELY while every
     * short string sailed through, which reads as a command-dispatch bug and
     * sent me auditing strcmp() twice before counting the string.
     *
     * Retrying the same oversized request does not help; the request itself has
     * to be smaller. 128 leaves room for whatever is already queued. */
    enum { CHUNK = 128 };
    size_t left = strlen(s);
    const uint8_t *p = (const uint8_t *)s;
    int stalls = 0;
    while (left > 0) {
        size_t want = left < CHUNK ? left : CHUNK;
        int n = usb_serial_jtag_write_bytes(p, want, pdMS_TO_TICKS(100));
        if (n > 0) { p += n; left -= (size_t)n; stalls = 0; continue; }
        if (++stalls > 20) break;      /* host gone; do not wedge the console */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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

    /* Network. An unprovisioned device says so in words rather than leaving a
     * blank for the reader to interpret. */
    ui_text(6, 244, "NETWORK", UI_DIM, UI_BG, 1);
    {
        char ip[32];
        if (craw_wifi_is_connected() && craw_wifi_get_ip_str(ip, sizeof ip)) {
            char ssid[CFG_MAX];
            cfg_get(CFG_WIFI_SSID, ssid, sizeof ssid);
            ui_text(6, 258, ssid[0] ? ssid : "WIFI", UI_GREEN, UI_BG, 1);
            ui_text(6, 272, ip, UI_WHITE, UI_BG, 1);
        } else if (cfg_get(CFG_WIFI_SSID, line, sizeof line)) {
            ui_text(6, 258, "CONNECTING", UI_AMBER, UI_BG, 1);
            ui_text(6, 272, line, UI_DIM, UI_BG, 1);
        } else {
            ui_text(6, 258, "NOT PROVISIONED", UI_DIM, UI_BG, 1);
            ui_text(6, 272, "SET WIFI SSID", UI_DIM, UI_BG, 1);
        }
    }

    ui_fill(0, UI_H - 3, UI_W, 3, UI_CYAN);
}

/* Redraw on every state change rather than polling: the status screen should
 * never be a stale claim about the network. */
static void wifi_event(craw_wifi_event_t ev, void *ctx) {
    unsigned long freeb = (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    switch (ev) {
    case CRAW_WIFI_EVENT_CONNECTED: {
        char ip[32] = "";
        craw_wifi_get_ip_str(ip, sizeof ip);
        usb_printf("[wifi] connected, ip %s, heap %lu\r\n", ip, freeb);
        draw_status("ONLINE", UI_GREEN, freeb);
        led_rgb(0, 40, 8);
        break;
    }
    case CRAW_WIFI_EVENT_DISCONNECTED:
        usb_print("[wifi] disconnected\r\n");
        draw_status("OFFLINE", UI_AMBER, freeb);
        led_rgb(40, 16, 0);
        break;
    case CRAW_WIFI_EVENT_CONNECT_FAILED:
        usb_print("[wifi] connect FAILED\r\n");
        draw_status("WIFI FAIL", UI_RED, freeb);
        led_rgb(48, 0, 0);
        break;
    }
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


    /* Vendor settings are authoritative now — no sweep. Status screen, and a
     * big F so a glance still confirms the glyph renderer is upright. */
    draw_status("IDLE", UI_GREEN, freeb);
    led_rgb(0, 24, 8);

    /* --- D1: connectivity ------------------------------------------------ */
    ESP_ERROR_CHECK(cfg_init());
    craw_wifi_init("wavec6led", wifi_event, NULL);
    usb_printf("heap after wifi init: %lu bytes\r\n",
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    {
        char ssid[CFG_MAX], pass[CFG_MAX];
        if (cfg_get(CFG_WIFI_SSID, ssid, sizeof ssid)) {
            cfg_get(CFG_WIFI_PASS, pass, sizeof pass);
            usb_printf("connecting to %s...\r\n", ssid);
            draw_status("WIFI", UI_AMBER,
                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            led_rgb(40, 24, 0);
            craw_wifi_connect(ssid, pass);
        } else {
            usb_print("no wifi credentials — use: set wifi_ssid <name>\r\n");
        }
    }

    console_run(repl_getchar, repl_putchar, usb_print);
    forth_deinit();
}
