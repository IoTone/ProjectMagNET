/*
 * MagNET_Vitals_E4TH — Phase 2.
 *
 * Adds on top of Phase 1.5:
 *   - NVS init (craw_nvs)
 *   - WiFi STA (craw_wifi) with auto-connect on stored creds
 *   - BLE GATT provisioning (craw_ble_provision) advertised as "MagNET-vitals-<MAC4>"
 *   - ESPIDFORTH REPL with vitals + provisioning words registered
 *   - Vitals task (radar status → LED, periodic lux refresh) on a dedicated FreeRTOS task
 *   - app_main blocks in the Forth REPL — same pattern as the M5Stamp Blinky_E4TH project
 *
 * Forth words registered:
 *
 *   Vitals                              Stack effect      Notes
 *   ──────────────────────────────────  ─────────────     ─────────────────────────────────
 *   hr?                                 ( -- bpm )        rounded to integer
 *   br?                                 ( -- rpm )        rounded to integer
 *   presence?                           ( -- 0|1 )
 *   distance?                           ( -- cm )         0 if range_flag == 0
 *   lux?                                ( -- lux )        −1 if BH1750 unavailable
 *   targets?                            ( -- count )      detected target count
 *   vitals                              ( -- )            print full snapshot
 *   hr-history.                         ( -- )            print last 60 HR samples
 *   br-history.                         ( -- )            print last 60 BR samples
 *   mr60-status                         ( -- )            print radar fw + last frame age
 *   led-vitals                          ( on/off -- )     enable/disable HR-hue LED feedback
 *
 *   Provisioning (copied verbatim from M5Stamp Blinky_E4TH):
 *   prov-status                         ( -- )
 *   prov-reset                          ( -- )
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "sdkconfig.h"

#include "craw_mr60bha2.h"
#include "craw_status_led.h"
#include "craw_bh1750.h"

#include "forth_core.h"
#include "craw_nvs.h"
#include "craw_wifi.h"
#include "craw_ble_provision.h"

#include "http_vitals.h"
#include "mdns.h"

static const char *TAG = "vitals";

#define FORTH_HEAP_SIZE  (64 * 1024)
#define LOOP_TICK_MS     50

#ifndef CONFIG_CRAW_MR60_UART_PORT
#define CONFIG_CRAW_MR60_UART_PORT 1
#endif
#ifndef CONFIG_CRAW_MR60_RX_GPIO
#define CONFIG_CRAW_MR60_RX_GPIO 17
#endif
#ifndef CONFIG_CRAW_MR60_TX_GPIO
#define CONFIG_CRAW_MR60_TX_GPIO 16
#endif
#ifndef CONFIG_CRAW_LED_GPIO
#define CONFIG_CRAW_LED_GPIO 21
#endif
#ifndef CONFIG_CRAW_BH1750_SDA_GPIO
#define CONFIG_CRAW_BH1750_SDA_GPIO 22
#endif
#ifndef CONFIG_CRAW_BH1750_SCL_GPIO
#define CONFIG_CRAW_BH1750_SCL_GPIO 23
#endif

/* ───── USB-serial-JTAG console (matches M5Stamp Blinky_E4TH pattern) ─── */

static void usb_print(const char *s) {
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(500));
}

static void usb_printf(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    usb_print(buf);
}

static int uart_getchar(void) {
    uint8_t c;
    int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
    return (n <= 0) ? -1 : c;
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

/* ───── mDNS — resolve `magnet-vitals.local` on the LAN ─────────────── */

#define VITALS_MDNS_HOSTNAME       "magnet-vitals"
#define VITALS_MDNS_INSTANCE_NAME  "MagNET Vitals"

static bool s_mdns_started = false;

static void start_mdns(void) {
    if (s_mdns_started) return;
    /* Idempotent across the WiFi-flap path: WiFi up → start; WiFi down →
     * leave running (mdns can be reused; no need to free/reinit). */
    esp_err_t err = mdns_init();
    if (err != ESP_OK) { ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err)); return; }
    err = mdns_hostname_set(VITALS_MDNS_HOSTNAME);
    if (err != ESP_OK) { ESP_LOGW(TAG, "mdns_hostname_set: %s", esp_err_to_name(err)); return; }
    err = mdns_instance_name_set(VITALS_MDNS_INSTANCE_NAME);
    if (err != ESP_OK) ESP_LOGW(TAG, "mdns_instance_name_set: %s", esp_err_to_name(err));

    /* Advertise the HTTP server on port 80. If two MagNET-Vitals nodes are
     * on the same LAN they'll collide — add a MAC suffix to the hostname
     * if you ever need multiple devices simultaneously. */
    mdns_txt_item_t txt[] = {
        { "role",  "vitals" },
        { "model", "MR60BHA2" },
    };
    err = mdns_service_add(NULL, "_http", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_service_add: %s", esp_err_to_name(err));
        return;
    }
    s_mdns_started = true;
    usb_printf("[mDNS] http://%s.local/ resolved on the LAN\r\n", VITALS_MDNS_HOSTNAME);
}

/* ───── Provisioning state mirrored to BLE ──────────────────────────── */

static volatile int  s_prov_state  = 0;   /* mirrors craw_ble_prov_state_t */
static volatile bool s_wifi_online = false;  /* drives WIFI_OFFLINE LED override */
static char s_ip_str[20] = "N/A";

static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    (void)ctx;
    switch (event) {
        case CRAW_WIFI_EVENT_CONNECTED:
            craw_wifi_get_ip_str(s_ip_str, sizeof(s_ip_str));
            usb_printf("\r\n[WiFi] connected, IP: %s\r\n", s_ip_str);
            craw_ble_provision_set_ip(s_ip_str);
            craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTED);
            s_prov_state = CRAW_BLE_PROV_CONNECTED;
            s_wifi_online = true;
            start_mdns();
            if (http_vitals_start() == ESP_OK) {
                usb_printf("[HTTP] vitals server up at http://%s/  (or http://%s.local/)\r\n",
                           s_ip_str, VITALS_MDNS_HOSTNAME);
            }
            break;
        case CRAW_WIFI_EVENT_DISCONNECTED:
            usb_print("\r\n[WiFi] disconnected\r\n");
            strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
            craw_ble_provision_set_ip(s_ip_str);
            http_vitals_stop();
            s_wifi_online = false;
            break;
        case CRAW_WIFI_EVENT_CONNECT_FAILED:
            usb_print("\r\n[WiFi] connect failed\r\n");
            strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
            craw_ble_provision_set_ip(s_ip_str);
            craw_ble_provision_set_status(CRAW_BLE_PROV_FAILED);
            s_prov_state = CRAW_BLE_PROV_FAILED;
            s_wifi_online = false;
            break;
    }
}

static void on_prov_event(craw_ble_prov_state_t state,
                          const char *ssid, const char *pass, void *ctx) {
    (void)ctx;
    s_prov_state = (int)state;
    switch (state) {
        case CRAW_BLE_PROV_CREDS_RECEIVED:
            usb_printf("\r\n[PROV] creds received: ssid='%s' pass=%d chars\r\n",
                       ssid, (int)strlen(pass ? pass : ""));
            break;
        case CRAW_BLE_PROV_COMMIT_REQUESTED:
            if (!ssid || !ssid[0]) {
                usb_print("\r\n[PROV] commit with empty SSID, ignoring\r\n");
                break;
            }
            usb_printf("\r\n[PROV] commit -> connecting to '%s'\r\n", ssid);
            craw_nvs_save_wifi_creds(ssid, pass ? pass : "");
            craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
            s_prov_state = CRAW_BLE_PROV_CONNECTING;
            craw_wifi_connect(ssid, pass ? pass : "");
            break;
        default:
            break;
    }
}

/* ───── Vitals tick task (radar→LED + periodic lux) ─────────────────── */

static volatile bool s_led_enabled = true;
static volatile bool s_radar_ok    = false;   /* set by self-test; gates vitals_task LED control */
static int64_t s_lux_last_us = 0;
static float   s_lux_value   = 0.0f;
static bool    s_lux_valid   = false;

static void vitals_task(void *arg) {
    (void)arg;
    /* Radar warm-up. */
    vTaskDelay(pdMS_TO_TICKS(2500));

    for (;;) {
        craw_mr60_state_t s;
        craw_mr60_get_state(&s);

        if (!s_led_enabled) {
            craw_status_led_set_mode(CRAW_LED_OFF, 0);
        } else if (!s_radar_ok) {
            /* Self-test never passed (or rebind in progress) — leave the LED
             * in its current TEST_FAIL flashing-red pattern. Don't overwrite. */
        } else if (!s_wifi_online) {
            /* Radar's fine but WiFi is down (never connected, lost link, or
             * all auth attempts failed). Yellow rapid-blink preempts the
             * radar-driven LED so the offline state is visible at a glance —
             * it's the most common reason the unit appears "stuck" without
             * the user knowing why. */
            craw_status_led_set_mode(CRAW_LED_WIFI_OFFLINE, 0);
        } else if (s.present) {
            craw_status_led_set_mode(CRAW_LED_PRESENCE, (int)(s.bpm + 0.5f));
        } else {
            craw_status_led_set_mode(CRAW_LED_IDLE, 0);
        }
        craw_status_led_tick(LOOP_TICK_MS);

        int64_t now_us = esp_timer_get_time();
        if (now_us - s_lux_last_us >= 1000000) {
            s_lux_last_us = now_us;
            float lux = 0.0f;
            s_lux_valid = (craw_bh1750_read(&lux) == ESP_OK);
            if (s_lux_valid) s_lux_value = lux;
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
    }
}

/* ───── Forth FFI words ──────────────────────────────────────────────── */

static void w_hr(void)        { forth_push((intptr_t)(int)(craw_mr60_get_bpm() + 0.5f)); }
static void w_br(void)        { forth_push((intptr_t)(int)(craw_mr60_get_rpm() + 0.5f)); }
static void w_presence(void)  { forth_push(craw_mr60_get_presence() ? 1 : 0); }

static void w_distance(void) {
    craw_mr60_state_t s;
    craw_mr60_get_state(&s);
    int cm = (s.range_flag != 0) ? (int)(s.distance_m * 100.0f + 0.5f) : 0;
    forth_push(cm);
}

static void w_lux(void) {
    if (s_lux_valid)        forth_push((intptr_t)(int)(s_lux_value + 0.5f));
    else                    forth_push(-1);
}

static void w_targets(void) {
    craw_mr60_target_t t[CRAW_MR60_MAX_TARGETS];
    forth_push((intptr_t)craw_mr60_get_targets(t));
}

static void w_vitals(void) {
    craw_mr60_state_t s;
    craw_mr60_get_state(&s);
    usb_printf("\r\nHR:      %.1f bpm\r\n", s.bpm);
    usb_printf("BR:      %.1f rpm\r\n", s.rpm);
    usb_printf("Pres:    %s\r\n", s.present ? "yes" : "no");
    usb_printf("Dist:    %.2f m  (range_flag=%lu)\r\n", s.distance_m, (unsigned long)s.range_flag);
    usb_printf("Phase:   t=%.2f  b=%.2f  h=%.2f\r\n",
               s.total_phase, s.breath_phase, s.heart_phase);
    usb_printf("Targets: %u\r\n", (unsigned)s.target_count);
    for (size_t i = 0; i < s.target_count; i++) {
        usb_printf("   [%u] x=%.2f y=%.2f dop=%ld cluster=%ld\r\n",
                   (unsigned)i, s.targets[i].x_m, s.targets[i].y_m,
                   (long)s.targets[i].dop_index, (long)s.targets[i].cluster_index);
    }
    usb_printf("Lux:     %s%.1f\r\n", s_lux_valid ? "" : "?", s_lux_valid ? s_lux_value : 0.0f);
    usb_printf("FW:      0x%08lx\r\n", (unsigned long)s.fw_version);
}

static void w_hr_history(void) {
    static uint64_t t_ms[CRAW_MR60_HISTORY_LEN];
    static float    bpm[CRAW_MR60_HISTORY_LEN];
    size_t n = craw_mr60_get_hr_history(t_ms, bpm, CRAW_MR60_HISTORY_LEN);
    usb_printf("\r\nHR history (%u sample%s)\r\n", (unsigned)n, n == 1 ? "" : "s");
    for (size_t i = 0; i < n; i++) {
        usb_printf("  [%2u] %llu ms  %.1f bpm\r\n",
                   (unsigned)i, (unsigned long long)t_ms[i], bpm[i]);
    }
}

static void w_br_history(void) {
    static uint64_t t_ms[CRAW_MR60_HISTORY_LEN];
    static float    rpm[CRAW_MR60_HISTORY_LEN];
    size_t n = craw_mr60_get_rr_history(t_ms, rpm, CRAW_MR60_HISTORY_LEN);
    usb_printf("\r\nBR history (%u sample%s)\r\n", (unsigned)n, n == 1 ? "" : "s");
    for (size_t i = 0; i < n; i++) {
        usb_printf("  [%2u] %llu ms  %.1f rpm\r\n",
                   (unsigned)i, (unsigned long long)t_ms[i], rpm[i]);
    }
}

static void w_mr60_status(void) {
    craw_mr60_state_t s;
    craw_mr60_get_state(&s);
    int64_t age_ms = (s.any_frame_us > 0)
        ? (esp_timer_get_time() - s.any_frame_us) / 1000
        : -1;
    usb_printf("\r\nradar fw=0x%08lx  last_frame=%lld ms ago\r\n",
               (unsigned long)s.fw_version, (long long)age_ms);
}

/* ( -- ) Print parser diagnostics: bytes seen, frames OK, checksum fails, etc. */
static void w_mr60_diag(void) {
    craw_mr60_diagnostics_t d;
    craw_mr60_get_diagnostics(&d);
    usb_printf("\r\nradar parser diagnostics:\r\n");
    usb_printf("  bytes received:    %llu\r\n", (unsigned long long)d.bytes_received);
    usb_printf("  valid frames:      %llu\r\n", (unsigned long long)d.frames_valid);
    usb_printf("  header cksum fail: %lu\r\n",  (unsigned long)d.header_cksum_fail);
    usb_printf("  data cksum fail:   %lu\r\n",  (unsigned long)d.data_cksum_fail);
    usb_printf("  implausible len:   %lu\r\n",  (unsigned long)d.implausible_len);
    usb_printf("  unknown type:      %lu\r\n",  (unsigned long)d.unknown_type);
    usb_printf("  first frame at:    %lld us\r\n", (long long)d.first_frame_us);
}

/* ( -- ok? ) Re-run the 10s self-test. Drives the LED green on PASS / red flash on FAIL.
 *  Pushes 1 if the radar emitted a valid frame within the window, 0 otherwise. */
static void w_mr60_test(void) {
    usb_print("\r\nRe-running radar self-test (10s)…\r\n");
    bool ok = craw_mr60_self_test(10000);
    if (ok) {
        usb_print("  PASS\r\n");
        craw_status_led_set_mode(CRAW_LED_TEST_OK, 0);
        vTaskDelay(pdMS_TO_TICKS(2000));
        s_radar_ok = true;     /* hand LED control back to vitals_task (yellow/blue) */
    } else {
        usb_print("  FAIL — run `mr60-diag` to see counters\r\n");
        craw_status_led_set_mode(CRAW_LED_TEST_FAIL, 0);
        s_radar_ok = false;    /* keep flashing red until next successful test */
    }
    forth_push(ok ? 1 : 0);
}

/* ( rx tx -- ) Hot-swap radar UART pins without reflashing.
 * Useful when the kit's actual radar pins differ from the Kconfig defaults.
 * Example at the REPL:  17 16 mr60-rebind  → RX=GPIO17, TX=GPIO16
 *                       4  5  mr60-rebind  → RX=GPIO4,  TX=GPIO5  (try alt pins)
 * After rebinding, run `mr60-test` to verify whether bytes now arrive. */
static void w_mr60_rebind(void) {
    int tx = (int)forth_pop();
    int rx = (int)forth_pop();
    usb_printf("\r\nRebinding radar UART → port=%d  rx=GPIO%d  tx=GPIO%d\r\n",
               CONFIG_CRAW_MR60_UART_PORT, rx, tx);
    /* Drop OK flag so the LED stays in its current pattern until `mr60-test`
     * confirms the new pins actually produce frames. */
    s_radar_ok = false;
    craw_status_led_set_mode(CRAW_LED_TEST_FAIL, 0);
    craw_mr60_deinit();
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_err_t err = craw_mr60_init((uart_port_t)CONFIG_CRAW_MR60_UART_PORT, rx, tx);
    if (err == ESP_OK) {
        usb_print("  rebind OK — wait ~2s, then run `mr60-test` to verify\r\n");
    } else {
        usb_printf("  rebind FAILED: %s\r\n", esp_err_to_name(err));
    }
}

static void w_led_vitals(void) {
    intptr_t v = forth_pop();
    s_led_enabled = (v != 0);
    usb_printf("\r\nLED feedback %s\r\n", s_led_enabled ? "on" : "off");
}

static void w_prov_status(void) {
    static const char *labels[] = {
        "IDLE", "CREDS_RECEIVED", "COMMIT_REQUESTED",
        "CONNECTING", "CONNECTED", "FAILED",
    };
    int st = s_prov_state;
    if (st < 0 || st >= (int)(sizeof(labels) / sizeof(labels[0]))) st = 0;
    char ssid[33], pass[65];
    bool has_creds = craw_nvs_load_wifi_creds(ssid, pass);
    usb_printf("\r\nprov:  %s\r\n", labels[st]);
    usb_printf("ble:   %s\r\n", craw_ble_provision_device_name());
    usb_printf("wifi:  %s\r\n", craw_wifi_is_connected() ? "connected" : "down");
    usb_printf("ssid:  %s\r\n", has_creds ? ssid : "(none)");
    usb_printf("ip:    %s\r\n", s_ip_str);
}

static void w_prov_reset(void) {
    char ssid[33], pass[65];
    craw_nvs_clear_wifi_creds(ssid, pass);
    craw_wifi_disconnect();
    strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
    craw_ble_provision_set_ip(s_ip_str);
    craw_ble_provision_set_status(CRAW_BLE_PROV_IDLE);
    s_prov_state = CRAW_BLE_PROV_IDLE;
    craw_ble_provision_advertise();
    usb_print("\r\nProvisioning reset. Advertising.\r\n");
}

static void register_vitals_words(void) {
    forth_register_word("hr?",         w_hr);
    forth_register_word("br?",         w_br);
    forth_register_word("presence?",   w_presence);
    forth_register_word("distance?",   w_distance);
    forth_register_word("lux?",        w_lux);
    forth_register_word("targets?",    w_targets);
    forth_register_word("vitals",      w_vitals);
    forth_register_word("hr-history.", w_hr_history);
    forth_register_word("br-history.", w_br_history);
    forth_register_word("mr60-status", w_mr60_status);
    forth_register_word("mr60-diag",   w_mr60_diag);
    forth_register_word("mr60-test",   w_mr60_test);
    forth_register_word("mr60-rebind", w_mr60_rebind);
    forth_register_word("led-vitals",  w_led_vitals);
    forth_register_word("prov-status", w_prov_status);
    forth_register_word("prov-reset",  w_prov_reset);
}

/* ───── Entry point ──────────────────────────────────────────────────── */

void app_main(void) {
    setup_usb_serial();
    vTaskDelay(pdMS_TO_TICKS(500));

    usb_print("\r\n\r\n");
    usb_print("============================================\r\n");
    usb_print("  MagNET Vitals E4TH — Phase 3\r\n");
    usb_print("  Radar · LED · lux · WiFi/BLE · Forth · HTTP\r\n");
    usb_print("--------------------------------------------\r\n");
    usb_printf("  radar:  UART%d  rx=GPIO%d  tx=GPIO%d  @115200\r\n",
               CONFIG_CRAW_MR60_UART_PORT,
               CONFIG_CRAW_MR60_RX_GPIO, CONFIG_CRAW_MR60_TX_GPIO);
    usb_printf("  LED:    GPIO%d  (WS2812)\r\n", CONFIG_CRAW_LED_GPIO);
    usb_printf("  BH1750: sda=GPIO%d  scl=GPIO%d\r\n",
               CONFIG_CRAW_BH1750_SDA_GPIO, CONFIG_CRAW_BH1750_SCL_GPIO);
    usb_print("============================================\r\n");

    /* Hardware init. */
    if (craw_status_led_init(CONFIG_CRAW_LED_GPIO) == ESP_OK) {
        craw_status_led_set_mode(CRAW_LED_BOOTING, 0);
    } else {
        ESP_LOGE(TAG, "status LED init failed (continuing)");
    }

    esp_err_t lux_err = craw_bh1750_init(
        CONFIG_CRAW_BH1750_SDA_GPIO, CONFIG_CRAW_BH1750_SCL_GPIO);
    if (lux_err != ESP_OK) {
        ESP_LOGW(TAG, "bh1750 init failed: %s — continuing without lux",
                 esp_err_to_name(lux_err));
    }

    esp_err_t err = craw_mr60_init(
        (uart_port_t)CONFIG_CRAW_MR60_UART_PORT,
        CONFIG_CRAW_MR60_RX_GPIO, CONFIG_CRAW_MR60_TX_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "craw_mr60_init failed: %s", esp_err_to_name(err));
        craw_status_led_set_mode(CRAW_LED_ERROR, 0);
        /* Continue anyway — Forth REPL still runs so the user can probe. */
    }

    /* ─── Radar self-test ─────────────────────────────────────────────
     * Wait up to 10 s for the first valid frame. Drive the LED solid
     * green on success or 4 Hz red flash on failure, and print a
     * diagnostic table so the user can tell whether bytes are arriving
     * at all (wiring problem) vs arriving as garbage (baud / endianness
     * / wrong device). The radar typically takes ~2 s to start emitting.
     */
    usb_print("\r\nRadar self-test — waiting up to 10s for first frame…\r\n");
    bool ok = (err == ESP_OK) && craw_mr60_self_test(10000);
    craw_mr60_diagnostics_t diag;
    craw_mr60_get_diagnostics(&diag);
    usb_printf("  bytes received:    %llu\r\n", (unsigned long long)diag.bytes_received);
    usb_printf("  valid frames:      %llu\r\n", (unsigned long long)diag.frames_valid);
    usb_printf("  header cksum fail: %lu\r\n",  (unsigned long)diag.header_cksum_fail);
    usb_printf("  data cksum fail:   %lu\r\n",  (unsigned long)diag.data_cksum_fail);
    usb_printf("  implausible len:   %lu\r\n",  (unsigned long)diag.implausible_len);
    usb_printf("  unknown type:      %lu\r\n",  (unsigned long)diag.unknown_type);
    if (ok) {
        usb_print("  RESULT: PASS — radar wiring + protocol verified.\r\n\r\n");
        craw_status_led_set_mode(CRAW_LED_TEST_OK, 0);
        vTaskDelay(pdMS_TO_TICKS(2500));   /* hold green so the user sees it */
        s_radar_ok = true;                 /* unlock IDLE/PRESENCE in vitals_task */
    } else {
        usb_print("  RESULT: FAIL.\r\n");
        if (diag.bytes_received == 0) {
            usb_print("    No bytes from UART. Most likely cause:\r\n");
            usb_printf("      - Wrong UART pins (current: UART%d, RX=GPIO%d, TX=GPIO%d).\r\n",
                       CONFIG_CRAW_MR60_UART_PORT,
                       CONFIG_CRAW_MR60_RX_GPIO, CONFIG_CRAW_MR60_TX_GPIO);
            usb_print("      - Try alt pins at the REPL without reflashing:\r\n");
            usb_print("          16 17 mr60-rebind   ( swap RX/TX )\r\n");
            usb_print("          mr60-test           ( verify )\r\n");
            usb_print("      - Or persist via `idf.py menuconfig` → MagNET Vitals (MR60BHA2 driver).\r\n");
            usb_print("      - Confirm the kit is powered (5V LED on) and the radar's small\r\n");
            usb_print("        green/red status LED is lit.\r\n");
        } else if (diag.frames_valid == 0) {
            usb_print("    Bytes arriving but no valid frames. Most likely cause:\r\n");
            usb_print("      - Wrong baud (driver uses 115200 — Seeed default).\r\n");
            usb_print("      - RX/TX swapped, picking up echo of our own console.\r\n");
            usb_print("      - Different mmWave variant (MR60FDA2 etc.) — protocol differs.\r\n");
        }
        usb_print("    The Forth REPL still works; try `mr60-diag` periodically while you debug.\r\n\r\n");
        craw_status_led_set_mode(CRAW_LED_TEST_FAIL, 0);
        /* Stay flashing red until a successful re-init via `mr60-rebind` + `mr60-test`.
         * vitals_task respects s_radar_ok=false and won't overwrite the LED. */
        s_radar_ok = false;
    }

    /* Provisioning. */
    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    craw_wifi_init("MagNET-vitals", on_wifi_event, NULL);
    craw_ble_provision_config_t prov_cfg = {
        .name_prefix = "MagNET-vitals",
        .role        = "vitals",
    };
    craw_ble_provision_init(&prov_cfg, on_prov_event, NULL);
    usb_printf("BLE advertising as '%s'\r\n", craw_ble_provision_device_name());

    /* Forth. */
    forth_init(FORTH_HEAP_SIZE);
    register_vitals_words();
    usb_print("Forth engine initialized.\r\n");
    usb_printf("Free heap: %lu bytes\r\n\r\n", (unsigned long)esp_get_free_heap_size());

    /* Vitals task — owns the LED and the lux refresh cadence. */
    xTaskCreate(vitals_task, "vitals_task", 4096, NULL, 5, NULL);

    /* Auto-connect if creds are saved. */
    char ssid[33], pass[65];
    if (craw_nvs_load_wifi_creds(ssid, pass) && ssid[0]) {
        usb_printf("Stored WiFi '%s' — auto-connecting...\r\n", ssid);
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
        s_prov_state = CRAW_BLE_PROV_CONNECTING;
        craw_wifi_connect(ssid, pass);
    } else {
        usb_print("No stored WiFi. Connect via BLE to provision.\r\n");
    }

    usb_print("\r\nForth commands:\r\n");
    usb_print("  hr?          ( -- bpm )      heart rate\r\n");
    usb_print("  br?          ( -- rpm )      breathing rate\r\n");
    usb_print("  presence?    ( -- 0|1 )      person in cone\r\n");
    usb_print("  distance?    ( -- cm )       distance to target\r\n");
    usb_print("  lux?         ( -- lux )      ambient light\r\n");
    usb_print("  targets?     ( -- count )    detected targets\r\n");
    usb_print("  vitals       ( -- )          full snapshot\r\n");
    usb_print("  hr-history.  ( -- )          last 60 HR samples\r\n");
    usb_print("  br-history.  ( -- )          last 60 BR samples\r\n");
    usb_print("  mr60-status  ( -- )          radar fw + last frame age\r\n");
    usb_print("  mr60-diag    ( -- )          parser diagnostic counters\r\n");
    usb_print("  mr60-test    ( -- ok? )      re-run 10s radar self-test\r\n");
    usb_print("  mr60-rebind  ( rx tx -- )    hot-swap UART pins, e.g. 17 16 mr60-rebind\r\n");
    usb_print("  led-vitals   ( on/off -- )   enable/disable HR-hue LED\r\n");
    usb_print("  prov-status  ( -- )          BLE/WiFi state\r\n");
    usb_print("  prov-reset   ( -- )          clear creds, re-advertise\r\n");
    usb_print("\r\nType 'words' for the full vocabulary.\r\n\r\n");

    /* Forth REPL — blocks forever. */
    forth_repl(uart_getchar, uart_putchar);
    forth_deinit();
}
