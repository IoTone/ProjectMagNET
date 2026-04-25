/*
 * M5Capsule_Hive_Scribe — Role 4 (Scribe) of the MagNET hive.
 *
 * "A scribe's only job is to save data to its internal memory and recall it
 *  from shared memory if asked." — README design section.
 *
 * Hardware: M5Capsule (ESP32-S3FN8, 8 MB flash, no PSRAM, 250 mAh LiPo,
 * USB-C native serial-JTAG, single button G6, buzzer G2, microSD slot,
 * BMI270 IMU + BM8563 RTC on I2C bus G8/G40, IR TX G4).
 *
 * v1 scope (this firmware):
 *   - Standard MagNET bringup: BLE provisioning → WiFi → SNTP → hive join
 *     as role=scribe with caps ["scribe","kv-store"].
 *   - Local NVS-backed string KV store in namespace "scribe_kv".
 *   - REPL-driven Forth words: scribe-store, scribe-recall, scribe-list,
 *     scribe-erase, scribe-count.
 *   - Buzzer chirp on store/recall (audible "I remembered something").
 *
 * Deferred to Milestone C / future work:
 *   - Hive-wide shared-memory queries (R16/R17). The scribe will respond to
 *     network RECALL messages once the hive protocol gains a generic
 *     PUBLISH/QUERY message type. For now KV is local-only and accessed
 *     via the REPL (and any role bundle running on this node).
 *   - microSD overflow storage when NVS gets tight.
 *   - IMU/RTC wakeup-driven persistence (low-power "remember on motion").
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/ledc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "forth_core.h"
#include "forth_version.h"
#include "craw_nvs.h"
#include "craw_wifi.h"
#include "craw_ble_provision.h"
#include "craw_hive.h"

static const char *TAG = "scribe";

/* ---------- Pin map (M5Capsule) ---------- */
#define BTN_GPIO        6
#define BUZZER_GPIO     2
#define IR_TX_GPIO      4
/* I2C SDA=8, SCL=40 unused in v1 — reserved for IMU/RTC */

#define FORTH_HEAP_SIZE (32 * 1024)
#define TIME_SYNC_EPOCH_THRESHOLD 1577836800

#define SCRIBE_NS       "scribe_kv"
#define SCRIBE_KEY_MAX  15   /* ESP-IDF NVS hard limit */
#define SCRIBE_VAL_MAX  256  /* arbitrary; bump if needed */

/* ---------- Console (USB-serial-JTAG) ---------- */
static void console_init(void) {
    usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 512, .rx_buffer_size = 512 };
    usb_serial_jtag_driver_install(&cfg);
}
static void uprint(const char *s) {
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(500));
}
static void uprintf(const char *fmt, ...) {
    char buf[256]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    uprint(buf);
}
static int console_getchar(void) {
    uint8_t c; int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
    return n > 0 ? c : -1;
}
static void console_putchar(int c) {
    uint8_t ch = (uint8_t)c;
    usb_serial_jtag_write_bytes(&ch, 1, pdMS_TO_TICKS(100));
}

/* ---------- Buzzer (LEDC PWM tone) ---------- */
static bool s_buzzer_ready = false;

static void buzzer_init(void) {
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 2000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = BUZZER_GPIO,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&c);
    s_buzzer_ready = true;
}

static void buzz(uint16_t freq_hz, uint16_t duration_ms) {
    if (!s_buzzer_ready) return;
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 64);  /* ~25% duty */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ---------- Scribe KV store (NVS, string values) ---------- */

static int scribe_store(const char *key, const char *value) {
    if (!key || !*key || strlen(key) > SCRIBE_KEY_MAX)  return -1;
    if (!value || strlen(value) > SCRIBE_VAL_MAX)       return -2;
    nvs_handle_t h;
    if (nvs_open(SCRIBE_NS, NVS_READWRITE, &h) != ESP_OK) return -3;
    esp_err_t err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -4;
}

/* Returns 0 on success and writes value to out_buf (NUL-terminated).
 * Returns -1 if key missing, other negative on internal error. */
static int scribe_recall(const char *key, char *out_buf, size_t out_len) {
    if (!key || !out_buf || out_len == 0) return -2;
    nvs_handle_t h;
    if (nvs_open(SCRIBE_NS, NVS_READONLY, &h) != ESP_OK) {
        out_buf[0] = '\0';
        return -1;
    }
    size_t sz = out_len;
    esp_err_t err = nvs_get_str(h, key, out_buf, &sz);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return -1;
    return err == ESP_OK ? 0 : -3;
}

static int scribe_erase(const char *key) {
    nvs_handle_t h;
    if (nvs_open(SCRIBE_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : (err == ESP_ERR_NVS_NOT_FOUND ? -2 : -3);
}

/* Iterate every entry in the scribe namespace, calling cb for each. */
typedef void (*scribe_iter_cb_t)(const char *key, const char *value, void *ctx);
static int scribe_iter(scribe_iter_cb_t cb, void *ctx) {
    nvs_iterator_t it = NULL;
    int count = 0;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, SCRIBE_NS, NVS_TYPE_STR, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        char val[SCRIBE_VAL_MAX + 1] = {0};
        if (scribe_recall(info.key, val, sizeof(val)) == 0) {
            if (cb) cb(info.key, val, ctx);
            count++;
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    return count;
}

/* ---------- Hive bringup (mirror Echo pattern) ---------- */
static char     ip_str[20]              = "N/A";
static char     hostname[40]            = "magnet-scribe-0000";
static char     node_id[40]             = "MagNET-biologic-0000";
static bool     ble_teardown_requested  = false;
static bool     ble_torn_down           = false;
static bool     sntp_started            = false;
static bool     time_synced             = false;
static bool     mdns_published          = false;
static bool     hive_started            = false;

static void mdns_publish(void) {
    if (mdns_published) return;
    mdns_init();
    mdns_hostname_set(hostname);
    mdns_instance_name_set("MagNET Hive Scribe");
    /* Advertise a magnet-node service so peers know what role lives here. */
    mdns_txt_item_t txt[] = {
        { "role", "scribe" },
        { "caps", "scribe,kv-store" },
        { "ver",  "1" },
    };
    mdns_service_add(NULL, "_magnet-node", "_tcp", 0, txt, sizeof(txt)/sizeof(txt[0]));
    mdns_published = true;
    uprintf("[mDNS] %s.local advertised as scribe\r\n", hostname);
}

static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    (void)ctx;
    switch (event) {
    case CRAW_WIFI_EVENT_CONNECTED:
        craw_wifi_get_ip_str(ip_str, sizeof(ip_str));
        uprintf("\r\n[WiFi] connected, IP: %s\r\n", ip_str);
        craw_ble_provision_set_ip(ip_str);
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTED);
        craw_ble_provision_stop_advertising();
        ble_teardown_requested = true;
        if (!sntp_started) {
            esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sntp_cfg);
            sntp_started = true;
            uprint("[SNTP] sync kicked off\r\n");
        }
        break;
    case CRAW_WIFI_EVENT_DISCONNECTED:
        uprint("\r\n[WiFi] disconnected\r\n");
        strncpy(ip_str, "N/A", sizeof(ip_str));
        if (!ble_torn_down) {
            craw_ble_provision_set_ip(ip_str);
            craw_ble_provision_advertise();
        }
        break;
    case CRAW_WIFI_EVENT_CONNECT_FAILED:
        uprint("\r\n[WiFi] failed\r\n");
        if (!ble_torn_down) {
            craw_ble_provision_set_status(CRAW_BLE_PROV_FAILED);
            craw_ble_provision_advertise();
        }
        break;
    }
}

static void on_prov_event(craw_ble_prov_state_t state,
                          const char *ssid, const char *pass, void *ctx) {
    (void)ctx;
    switch (state) {
    case CRAW_BLE_PROV_CREDS_RECEIVED:
        uprintf("\r\n[PROV] creds: ssid='%s'\r\n", ssid);
        break;
    case CRAW_BLE_PROV_COMMIT_REQUESTED:
        if (!ssid || !ssid[0]) break;
        uprintf("\r\n[PROV] commit -> '%s'\r\n", ssid);
        craw_nvs_save_wifi_creds(ssid, pass ? pass : "");
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
        craw_wifi_connect(ssid, pass ? pass : "");
        break;
    default: break;
    }
}

static void on_hive_state(craw_hive_node_state_t state,
                          const char *info, void *ctx) {
    (void)ctx;
    uprintf("\r\n[HIVE] state=%d (%s)\r\n", (int)state, info ? info : "");
    if (state == CRAW_HIVE_NODE_JOINED) buzz(2000, 60);
}

static const char *CAPS[] = { "scribe", "kv-store", NULL };
static uint8_t      secret_copy[32];

static void derive_ids(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(hostname, sizeof(hostname), "magnet-scribe-%02x%02x", mac[4], mac[5]);
    snprintf(node_id,  sizeof(node_id),  "MagNET-biologic-%02x%02x", mac[4], mac[5]);
}

static void maybe_start_hive(void) {
    if (hive_started) return;
    if (!craw_wifi_is_connected()) return;
    if (!time_synced) return;
    derive_ids();
    if (!mdns_published) mdns_publish();
    memcpy(secret_copy, CRAW_HIVE_DEV_SECRET, 32);
    static craw_hive_node_config_t ncfg;
    ncfg = (craw_hive_node_config_t){
        .node_id        = node_id,
        .hive_id        = "beehive-1",
        .role_requested = "scribe",
        .caps           = CAPS,
        .chip           = "ESP32-S3",
        .fw             = "0.1.0",
        .secret         = secret_copy,
        .on_state       = on_hive_state,
        .on_state_ctx   = NULL,
    };
    if (craw_hive_node_start(&ncfg) == 0) {
        hive_started = true;
        uprint("[HIVE] node started as role=scribe caps=scribe,kv-store\r\n");
    }
}

static void housekeeping_task(void *arg) {
    (void)arg;
    while (1) {
        if (ble_teardown_requested && !ble_torn_down) {
            vTaskDelay(pdMS_TO_TICKS(500));
            size_t before = esp_get_free_heap_size();
            craw_ble_provision_deinit();
            ble_torn_down = true;
            size_t after = esp_get_free_heap_size();
            uprintf("[BLE] torn down. Heap: %u -> %u (+%d bytes)\r\n",
                    (unsigned)before, (unsigned)after,
                    (int)after - (int)before);
        }
        if (sntp_started && !time_synced) {
            sntp_sync_status_t st = sntp_get_sync_status();
            time_t now = time(NULL);
            if (st == SNTP_SYNC_STATUS_COMPLETED && now > TIME_SYNC_EPOCH_THRESHOLD) {
                time_synced = true;
                struct tm t; localtime_r(&now, &t);
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
                uprintf("[SNTP] time synced: %s UTC\r\n", buf);
            }
        }
        maybe_start_hive();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* ---------- Forth FFI ---------- */

/* Forth doesn't have native string support. The KV words take addresses of
 * NUL-terminated C strings on the stack — typically you'd push pointers
 * from within a role bundle. For interactive REPL use, line-edit the
 * key/value via a one-shot `prompt-store`/`prompt-recall` helper. */

static char s_io_key[SCRIBE_KEY_MAX + 1];
static char s_io_val[SCRIBE_VAL_MAX + 1];

static void read_line(const char *prompt, char *buf, size_t bufsz) {
    uprint(prompt);
    size_t i = 0;
    while (i + 1 < bufsz) {
        int c = console_getchar();
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (c == '\r' || c == '\n') break;
        if (c == 8 || c == 127) {
            if (i > 0) { i--; uprint("\b \b"); }
            continue;
        }
        buf[i++] = (char)c;
        char ch[2] = { (char)c, 0 };
        uprint(ch);
    }
    buf[i] = '\0';
    uprint("\r\n");
}

/* ( -- ) Interactive: prompts for key + value, stores. */
static void w_scribe_store(void) {
    read_line("\r\nkey: ",   s_io_key, sizeof(s_io_key));
    read_line("value: ",     s_io_val, sizeof(s_io_val));
    int rc = scribe_store(s_io_key, s_io_val);
    if (rc == 0) {
        uprintf("stored '%s' = '%s'\r\n", s_io_key, s_io_val);
        buzz(1500, 30);
    } else {
        uprintf("scribe-store failed rc=%d\r\n", rc);
    }
}

/* ( -- ) Interactive: prompts for key, prints stored value. */
static void w_scribe_recall(void) {
    read_line("\r\nkey: ", s_io_key, sizeof(s_io_key));
    int rc = scribe_recall(s_io_key, s_io_val, sizeof(s_io_val));
    if (rc == 0) {
        uprintf("'%s' = '%s'\r\n", s_io_key, s_io_val);
        buzz(2200, 30);
    } else if (rc == -1) {
        uprintf("not found\r\n");
    } else {
        uprintf("scribe-recall failed rc=%d\r\n", rc);
    }
}

static void list_cb(const char *key, const char *value, void *ctx) {
    (void)ctx;
    uprintf("  %-15s = %s\r\n", key, value);
}

static void w_scribe_list(void) {
    uprint("\r\nscribe entries:\r\n");
    int n = scribe_iter(list_cb, NULL);
    uprintf("(%d entries)\r\n", n);
}

static void w_scribe_erase(void) {
    read_line("\r\nkey to erase: ", s_io_key, sizeof(s_io_key));
    int rc = scribe_erase(s_io_key);
    if      (rc == 0)  uprintf("erased '%s'\r\n", s_io_key);
    else if (rc == -2) uprint("not found\r\n");
    else               uprintf("scribe-erase failed rc=%d\r\n", rc);
}

static void w_scribe_count(void) {
    int n = scribe_iter(NULL, NULL);
    forth_push(n);
    uprintf("\r\nscribe entries: %d\r\n", n);
}

static void w_prov_status(void) {
    char ssid[33], pass[65];
    bool has = craw_nvs_load_wifi_creds(ssid, pass);
    uprintf("\r\nble:    %s\r\n", craw_ble_provision_device_name());
    uprintf("wifi:   %s\r\n", craw_wifi_is_connected() ? "connected" : "down");
    uprintf("ssid:   %s\r\n", has ? ssid : "(none)");
    uprintf("ip:     %s\r\n", ip_str);
    uprintf("time:   %s\r\n", time_synced ? "synced" : "pending");
    uprintf("host:   %s.local\r\n", hostname);
    uprintf("hive:   %s\r\n", hive_started ? "joined" : "idle");
    int n = scribe_iter(NULL, NULL);
    uprintf("scribe: %d entries in NVS\r\n", n);
}

static void w_prov_reset(void) {
    char ssid[33], pass[65];
    craw_nvs_clear_wifi_creds(ssid, pass);
    uprint("\r\nWiFi creds cleared. Rebooting to re-enter provisioning...\r\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void w_hive_status(void) {
    const char *labels[] = { "OFFLINE","DISCOVER","CONNECTING","JOINED","BACKOFF" };
    int st = (int)craw_hive_node_state();
    if (st < 0 || st > 4) st = 0;
    const char *sid = craw_hive_node_session_id();
    uprintf("\r\nhive:    %s\r\n", labels[st]);
    uprintf("node:    %s\r\n", node_id);
    uprintf("session: %s\r\n", sid ? sid : "(none)");
}

static void w_buzz(void) {
    int dur = (int)forth_pop();
    int hz  = (int)forth_pop();
    if (hz <= 0)  return;
    if (dur < 1)  dur = 1;
    if (dur > 2000) dur = 2000;
    buzz((uint16_t)hz, (uint16_t)dur);
}

static void register_forth_words(void) {
    forth_register_word("scribe-store",  w_scribe_store);
    forth_register_word("scribe-recall", w_scribe_recall);
    forth_register_word("scribe-list",   w_scribe_list);
    forth_register_word("scribe-erase",  w_scribe_erase);
    forth_register_word("scribe-count",  w_scribe_count);
    forth_register_word("prov-status",   w_prov_status);
    forth_register_word("prov-reset",    w_prov_reset);
    forth_register_word("hive-status",   w_hive_status);
    forth_register_word("buzz",          w_buzz);
}

/* ---------- app_main ---------- */
void app_main(void) {
    console_init();
    vTaskDelay(pdMS_TO_TICKS(500));

    uprint("\r\n\r\n====================================\r\n");
    uprint(  "  M5Capsule Hive Scribe (Role 4)\r\n");
    uprintf( "  ESPIDFORTH %s\r\n", ESPIDFORTH_VERSION_STRING);
    uprint(  "  Persistent KV store + hive node\r\n");
    uprint(  "====================================\r\n");

    buzzer_init();
    buzz(1200, 50);
    buzz(1800, 50);   /* boot chirp */

    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    craw_wifi_init("MagNET-biologic", on_wifi_event, NULL);

    craw_ble_provision_config_t pcfg = {
        .name_prefix = "MagNET-biologic",
        .role        = "scribe",
    };
    craw_ble_provision_init(&pcfg, on_prov_event, NULL);
    uprintf("BLE: %s\r\n", craw_ble_provision_device_name());

    forth_init(FORTH_HEAP_SIZE);
    register_forth_words();
    uprintf("Forth ready. Free heap: %lu bytes\r\n",
            (unsigned long)esp_get_free_heap_size());

    xTaskCreate(housekeeping_task, "keep", 6144, NULL, 3, NULL);

    char ssid[33], pass[65];
    if (craw_nvs_load_wifi_creds(ssid, pass) && ssid[0]) {
        uprintf("Stored WiFi '%s' — auto-connect\r\n", ssid);
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
        craw_wifi_connect(ssid, pass);
    } else {
        uprint("No stored WiFi — provision via BLE.\r\n");
    }

    uprint("\r\nForth commands:\r\n");
    uprint("  scribe-store / scribe-recall / scribe-list\r\n");
    uprint("  scribe-erase / scribe-count\r\n");
    uprint("  prov-status / prov-reset / hive-status\r\n");
    uprint("  HZ DUR_MS buzz\r\n\r\n");

    forth_repl(console_getchar, console_putchar);
    forth_deinit();
}
