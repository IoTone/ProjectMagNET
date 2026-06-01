/*
 * MagNET_ReSpeaker_Boombox — Role 12 (Boombox).
 *
 * "Any speakers or audio playback device. Different than the beeper, it
 *  can play full sounds, music, or recordings." — README design section.
 *
 * Hardware: Seeed XIAO ESP32-S3 socketed onto the ReSpeaker Lite Voice
 * Kit carrier. v1 uses the carrier's I2S audio path only — the XMOS
 * mic-array side is not consumed (would belong to a future Eye/listener
 * role). USB-C native serial-JTAG console.
 *
 * v1 scope:
 *   - Standard MagNET bringup: BLE provisioning → WiFi → SNTP → BLE
 *     teardown → hive join as role=boombox, caps=["audio","speaker","tone"].
 *   - Software synth (sine LUT) at 16 kHz / 16-bit / mono via I2S0.
 *   - Forth words for tone / sweep / am / sleep / canned sounds /
 *     volume / amp / status / stop.
 *   - Hive integration:
 *       * accept ROLE_GRANT bundles (existing craw_role_bundle pipeline) —
 *         a bundle can register new Forth words (e.g. : sos ... ;) that
 *         compose the audio primitives.
 *       * poll KV key "boombox:cmd"; if non-empty, run the value as a
 *         Forth phrase, then clear it. Lets the Dial trigger sounds with
 *           s" alert" s" boombox:cmd" kv-set
 *       * publish "boombox:status" heartbeat every 5 s for a future Dial
 *         indicator dot.
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
#include "driver/usb_serial_jtag.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "forth_core.h"
#include "forth_version.h"
#include "craw_nvs.h"
#include "craw_wifi.h"
#include "craw_ble_provision.h"
#include "craw_hive.h"
#include "craw_role_bundle.h"
#include "craw_audio.h"
#include "http_speaker.h"
#include "../../include/magnet_gen.h"

static const char *TAG = "boombox";

/* ---------- Pin map ---------- *
 * Confirmed against the Seeed reference example
 * `xiao_esp32s3_arduino_examples/xiao_i2c_control_volume.ino` in the
 * ReSpeaker_Lite repo. The XU316 chip on the carrier generates BCLK/WS
 * and the ESP32-S3 acts as I2S slave on the TX side. DATA on GPIO 43
 * (XIAO's TX0 pin) is the speaker-bound stream into the XU316/codec.
 */
#define I2S_BCLK_GPIO   8
#define I2S_WS_GPIO     7
#define I2S_DOUT_GPIO   43
#define AMP_PWR_EN_GPIO -1   /* carrier handles amp power autonomously */

#define FORTH_HEAP_SIZE  (32 * 1024)
#define TIME_SYNC_EPOCH_THRESHOLD 1577836800

#define BOOMBOX_NS       "boombox"
#define BOOMBOX_KEY_VOL  "vol"

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

/* ---------- NVS for boombox-specific settings ---------- */
static void boombox_nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open(BOOMBOX_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = (uint8_t)craw_audio_volume_get();
    nvs_get_u8(h, BOOMBOX_KEY_VOL, &v);
    craw_audio_volume_set(v);
    nvs_close(h);
}
static void boombox_nvs_save_vol(int v) {
    nvs_handle_t h;
    if (nvs_open(BOOMBOX_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, BOOMBOX_KEY_VOL, (uint8_t)v);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------- Hive bringup state ---------- */
static char ip_str[20]              = "N/A";
static char hostname[40]            = "magnet-boombox-0000";
static char node_id[40]             = "MagNET-biologic-0000";
static bool ble_teardown_requested  = false;
static bool ble_torn_down           = false;
static bool sntp_started            = false;
static bool time_synced             = false;
static bool mdns_published          = false;
static bool hive_started            = false;

static void mdns_publish(void) {
    if (mdns_published) return;
    mdns_init();
    mdns_hostname_set(hostname);
    mdns_instance_name_set("MagNET Hive Boombox");
    mdns_txt_item_t txt[] = {
        { "role", "boombox" },
        { "caps", "audio,speaker,tone" },
        { "ver",  "1" },
    };
    mdns_service_add(NULL, "_magnet-node", "_tcp", 0, txt, sizeof(txt)/sizeof(txt[0]));
    mdns_published = true;
    uprintf("[mDNS] %s.local advertised as boombox\r\n", hostname);
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
        /* UC2 speaker actuator API — independent of hive bringup, ready
         * as soon as WiFi is up so the dataspace can ring chime/doorbell. */
        if (http_speaker_start() == ESP_OK)
            uprintf("[HTTP] http://%s/api/v1/actuator/speaker/play\r\n", ip_str);
        break;
    case CRAW_WIFI_EVENT_DISCONNECTED:
        uprint("\r\n[WiFi] disconnected\r\n");
        strncpy(ip_str, "N/A", sizeof(ip_str));
        http_speaker_stop();
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
    if (state == CRAW_HIVE_NODE_JOINED) craw_audio_play_notify();
}

static const char *NODE_CAPS[] = { "audio", "speaker", "tone" };
#define N_NODE_CAPS (sizeof(NODE_CAPS) / sizeof(NODE_CAPS[0]))

typedef struct {
    char bundle_key[CRAW_HIVE_KV_KEY_MAX + 1];
    char role[CRAW_HIVE_ROLE_MAX + 1];
} bundle_install_job_t;

static void bundle_install_worker(void *arg) {
    bundle_install_job_t *job = (bundle_install_job_t *)arg;
    char *json_buf = malloc(CRAW_HIVE_KV_VALUE_MAX + 1);
    if (!json_buf) {
        uprintf("\r\n[bundle] worker malloc failed\r\n");
        free(job); vTaskDelete(NULL); return;
    }
    uprintf("\r\n[bundle] fetching %s for role=%s...\r\n", job->bundle_key, job->role);
    int rc = craw_hive_node_kv_get(job->bundle_key, json_buf,
                                   CRAW_HIVE_KV_VALUE_MAX + 1, 5000);
    if (rc == 0) {
        craw_role_bundle_install_result_t result = {0};
        int irc = craw_role_bundle_install_from_json(json_buf,
                                                     NODE_CAPS, N_NODE_CAPS,
                                                     &result);
        if (irc == BUNDLE_OK) {
            uprintf("[bundle] '%s' v%s installed (%u bytes src)\r\n",
                    result.info.name, result.info.version,
                    (unsigned)result.info.src_len);
            craw_audio_play_notify();
        } else {
            uprintf("[bundle] install failed rc=%d field='%s'\r\n",
                    irc, result.err_field);
            craw_audio_play_error();
        }
    } else {
        uprintf("[bundle] kv-get '%s' failed rc=%d\r\n", job->bundle_key, rc);
    }
    free(json_buf); free(job); vTaskDelete(NULL);
}

static void on_role_grant(const char *role, const char *bundle_key,
                          const char *scribe, void *ctx) {
    (void)ctx; (void)scribe;
    uprintf("\r\n[ROLE_GRANT] role=%s bundle=%s\r\n",
            role, bundle_key ? bundle_key : "(none)");
    if (!bundle_key) return;
    bundle_install_job_t *job = malloc(sizeof(*job));
    if (!job) return;
    strncpy(job->role, role, sizeof(job->role) - 1);
    job->role[sizeof(job->role) - 1] = '\0';
    strncpy(job->bundle_key, bundle_key, sizeof(job->bundle_key) - 1);
    job->bundle_key[sizeof(job->bundle_key) - 1] = '\0';
    if (xTaskCreate(bundle_install_worker, "bundle_inst", 8192, job, 4, NULL) != pdPASS) {
        free(job);
    }
}

static const char *CAPS[] = { "audio", "speaker", "tone", NULL };
static uint8_t      secret_copy[32];

static void derive_ids(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(hostname, sizeof(hostname), "magnet-boombox-%02x%02x", mac[4], mac[5]);
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
        .node_id          = node_id,
        .hive_id          = "beehive-1",
        .role_requested   = "boombox",
        .caps             = CAPS,
        .chip             = "ESP32-S3",
        .fw               = "0.1.0",
        .gen              = MAGNET_GEN_STR,
        .secret           = secret_copy,
        .on_state         = on_hive_state,
        .on_state_ctx     = NULL,
        .on_role_grant    = on_role_grant,
        .on_role_grant_ctx= NULL,
    };
    if (craw_hive_node_start(&ncfg) == 0) {
        hive_started = true;
        uprint("[HIVE] node started as role=boombox\r\n");
    }
}

/* ---------- Boombox command poll ----------
 *
 * Every 2 s while joined, fetch KV "boombox:cmd". If non-empty, evaluate
 * the value as a Forth phrase (so "alert", "notify", "1500 100 tone",
 * or any registered Forth word works), then clear the key. Lets any
 * other hive node trigger sounds without speaking RESP / MQTT.
 */
static void cmd_poll_task(void *arg) {
    (void)arg;
    char val[128];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!craw_hive_node_session_id()) continue;
        int rc = craw_hive_node_kv_get("boombox:cmd", val, sizeof(val), 1500);
        if (rc != 0 || !val[0]) continue;
        uprintf("\r\n[cmd] %s\r\n", val);
        forth_eval(val);
        craw_hive_node_kv_put("boombox:cmd", "");   /* one-shot */
    }
}

/* ---------- Heartbeat (status KV for Dial dot) ---------- */
static void hb_task(void *arg) {
    (void)arg;
    while (1) {
        if (craw_hive_node_session_id()) {
            craw_audio_stats_t st; craw_audio_stats(&st);
            char val[80];
            snprintf(val, sizeof(val), "%s:%d:%s",
                     node_id, st.volume_pct, st.amp_on ? "on" : "off");
            craw_hive_node_kv_put("boombox:status", val);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ---------- Housekeeping (mirrors Capsule scribe) ---------- */
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

static char s_io_a[64];

static void read_line(const char *prompt, char *buf, size_t bufsz) {
    uprint(prompt);
    size_t i = 0;
    while (i + 1 < bufsz) {
        int c = console_getchar();
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (c == '\r' || c == '\n') break;
        if (c == 8 || c == 127) { if (i) { i--; uprint("\b \b"); } continue; }
        buf[i++] = (char)c;
        char ch[2] = { (char)c, 0 }; uprint(ch);
    }
    buf[i] = '\0';
    uprint("\r\n");
}

/* Default per-segment gain when triggered from the REPL. The audio
 * subsystem then scales by master volume. 0.7 is loud-but-not-clipping. */
#define DEFAULT_GAIN 0.7f

/* ( freq dur -- ) */
static void w_tone(void) {
    int dur = (int)forth_pop();
    int hz  = (int)forth_pop();
    if (hz <= 0 || dur <= 0) return;
    craw_audio_play_tone((uint16_t)hz, (uint16_t)dur, DEFAULT_GAIN);
}
/* ( f0 f1 dur -- ) */
static void w_sweep(void) {
    int dur = (int)forth_pop();
    int f1  = (int)forth_pop();
    int f0  = (int)forth_pop();
    if (dur <= 0 || f0 <= 0 || f1 <= 0) return;
    craw_audio_play_sweep((uint16_t)f0, (uint16_t)f1, (uint16_t)dur, DEFAULT_GAIN);
}
/* ( fc fm dur -- ) */
static void w_am(void) {
    int dur = (int)forth_pop();
    int fm  = (int)forth_pop();
    int fc  = (int)forth_pop();
    if (dur <= 0 || fc <= 0 || fm <= 0) return;
    craw_audio_play_am((uint16_t)fc, (uint16_t)fm, (uint16_t)dur, DEFAULT_GAIN);
}
/* ( ms -- ) */
static void w_sleep(void) {
    int ms = (int)forth_pop();
    if (ms <= 0) return;
    craw_audio_play_sleep((uint16_t)ms);
}

/* Canned recipes — zero-arg. */
static void w_alert    (void) { craw_audio_play_alert();    }
static void w_notify   (void) { craw_audio_play_notify();   }
static void w_warn     (void) { craw_audio_play_warn();     }
static void w_error    (void) { craw_audio_play_error();    }
static void w_siren    (void) { craw_audio_play_siren();    }
static void w_yelp     (void) { craw_audio_play_yelp();     }
static void w_nee_naw  (void) { craw_audio_play_nee_naw();  }
static void w_air_raid (void) { craw_audio_play_air_raid(); }
static void w_sunrise  (void) { craw_audio_play_sunrise();  }

/* ( n -- ) */
static void w_vol_set(void) {
    int v = (int)forth_pop();
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    craw_audio_volume_set(v);
    boombox_nvs_save_vol(v);
    uprintf("\r\nvolume = %d\r\n", v);
}
static void w_vol_get(void) {
    int v = craw_audio_volume_get();
    forth_push(v);
    uprintf("\r\nvolume: %d\r\n", v);
}
static void w_audio_on   (void) { craw_audio_amp_set(true);  uprint("\r\namp on\r\n");  }
static void w_audio_off  (void) { craw_audio_amp_set(false); uprint("\r\namp off\r\n"); }
static void w_audio_stop (void) { craw_audio_stop(); uprint("\r\naudio stopped\r\n");  }
static void w_audio_status(void) {
    craw_audio_stats_t st; craw_audio_stats(&st);
    uprintf("\r\namp:       %s\r\n",      st.amp_on ? "on" : "off");
    uprintf("volume:    %d\r\n",          st.volume_pct);
    uprintf("rendering: %s\r\n",          st.rendering ? "yes" : "no");
    uprintf("queue:     %d / %d\r\n",     st.queue_depth, CRAW_AUDIO_QUEUE_DEPTH);
    uprintf("segments:  %u\r\n",          (unsigned)st.total_segments_played);
    uprintf("samples:   %llu\r\n",        (unsigned long long)st.total_samples_out);
}

static void w_prov_status(void) {
    char ssid[33], pass[65];
    bool has = craw_nvs_load_wifi_creds(ssid, pass);
    uprintf("\r\nble:    %s\r\n",  craw_ble_provision_device_name());
    uprintf("wifi:   %s\r\n",      craw_wifi_is_connected() ? "connected" : "down");
    uprintf("ssid:   %s\r\n",      has ? ssid : "(none)");
    uprintf("ip:     %s\r\n",      ip_str);
    uprintf("time:   %s\r\n",      time_synced ? "synced" : "pending");
    uprintf("host:   %s.local\r\n", hostname);
    uprintf("hive:   %s\r\n",      hive_started ? "joined" : "idle");
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
    uprintf("node:    %s\r\n",     node_id);
    uprintf("session: %s\r\n",     sid ? sid : "(none)");
}
static void w_hive_kv_get(void) {
    read_line("\r\nkey: ", s_io_a, sizeof(s_io_a));
    char val[256];
    int rc = craw_hive_node_kv_get(s_io_a, val, sizeof(val), 3000);
    if      (rc == 0)  uprintf("'%s' = '%s'\r\n", s_io_a, val);
    else if (rc == 1)  uprint("not found\r\n");
    else if (rc == -2) uprint("timeout\r\n");
    else               uprintf("kv-get failed rc=%d\r\n", rc);
}
static void w_hive_kv_put(void) {
    char k[CRAW_HIVE_KV_KEY_MAX + 1], v[256];
    read_line("\r\nkey:   ", k, sizeof(k));
    read_line("value: ",     v, sizeof(v));
    int rc = craw_hive_node_kv_put(k, v);
    uprintf("kv-put rc=%d\r\n", rc);
}

static void register_forth_words(void) {
    forth_register_word("tone",          w_tone);
    forth_register_word("sweep",         w_sweep);
    forth_register_word("am",            w_am);
    forth_register_word("sleep",         w_sleep);
    forth_register_word("alert",         w_alert);
    forth_register_word("notify",        w_notify);
    forth_register_word("warn",          w_warn);
    forth_register_word("error",         w_error);
    forth_register_word("siren",         w_siren);
    forth_register_word("yelp",          w_yelp);
    forth_register_word("nee-naw",       w_nee_naw);
    forth_register_word("air-raid",      w_air_raid);
    forth_register_word("sunrise",       w_sunrise);
    forth_register_word("vol",           w_vol_set);
    forth_register_word("vol?",          w_vol_get);
    forth_register_word("audio-on",      w_audio_on);
    forth_register_word("audio-off",     w_audio_off);
    forth_register_word("audio-stop",    w_audio_stop);
    forth_register_word("audio-status",  w_audio_status);
    forth_register_word("prov-status",   w_prov_status);
    forth_register_word("prov-reset",    w_prov_reset);
    forth_register_word("hive-status",   w_hive_status);
    forth_register_word("kv-get",        w_hive_kv_get);
    forth_register_word("kv-put",        w_hive_kv_put);
}

/* ---------- app_main ---------- */
void app_main(void) {
    console_init();
    vTaskDelay(pdMS_TO_TICKS(500));

    uprint("\r\n\r\n====================================\r\n");
    uprint(  "  MagNET ReSpeaker Boombox (Role 12)\r\n");
    uprintf( "  ESPIDFORTH %s\r\n", ESPIDFORTH_VERSION_STRING);
    uprintf( "  MagNET gen %s\r\n", MAGNET_GEN_STR);
    uprint(  "  XIAO ESP32-S3 + ReSpeaker Lite Voice Kit\r\n");
    uprint(  "====================================\r\n");

    /* Audio first so we can play the sunrise chirp at the very end. */
    craw_audio_pins_t pins = {
        .bclk = I2S_BCLK_GPIO,
        .ws   = I2S_WS_GPIO,
        .dout = I2S_DOUT_GPIO,
        .pwr_en = AMP_PWR_EN_GPIO,
        .pwr_en_active_high = true,
    };
    int arc = craw_audio_init(&pins);
    if (arc != 0) {
        uprintf("[audio] init failed rc=%d — boombox will run silent\r\n", arc);
    } else {
        craw_audio_amp_set(true);
        uprint("[audio] I2S0 ready\r\n");
    }

    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    boombox_nvs_load();    /* volume */

    /* Forth FIRST: claim the dictionary heap while RAM is plentiful. Same
     * lesson as XIAO_ESP32C3_IOT_LIGHTING — NimBLE + WiFi + httpd grab
     * large internal buffers that fragment the heap and starve forth_init,
     * silently truncating ESP32forth core + the bundle. */
    forth_init(FORTH_HEAP_SIZE);
    register_forth_words();
    uprintf("Forth ready. Free heap: %lu bytes\r\n",
            (unsigned long)esp_get_free_heap_size());

    craw_wifi_init("MagNET-biologic", on_wifi_event, NULL);

    craw_ble_provision_config_t pcfg = {
        .name_prefix = "MagNET-biologic",
        .role        = "boombox",
        /* BT SIG Generic Audio Source — drives the speaker icon in BLE
         * scanners (nRF Connect etc.). See specs/UDM-MagNET-v1.md §10.8. */
        .appearance  = 0x0440,
    };
    craw_ble_provision_init(&pcfg, on_prov_event, NULL);
    uprintf("BLE: %s\r\n", craw_ble_provision_device_name());

    craw_role_bundle_init();
    int reapplied = craw_role_bundle_apply_saved(NODE_CAPS, N_NODE_CAPS);
    if (reapplied > 0) {
        uprintf("[bundle] re-applied %d persisted bundle(s) from NVS\r\n",
                reapplied);
    }

    xTaskCreate(housekeeping_task, "keep",    6144, NULL, 3, NULL);
    xTaskCreate(cmd_poll_task,     "cmd",     4096, NULL, 3, NULL);
    xTaskCreate(hb_task,           "boom_hb", 3072, NULL, 3, NULL);

    char ssid[33], pass[65];
    if (craw_nvs_load_wifi_creds(ssid, pass) && ssid[0]) {
        uprintf("Stored WiFi '%s' — auto-connect\r\n", ssid);
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
        craw_wifi_connect(ssid, pass);
    } else {
        uprint("No stored WiFi — provision via BLE.\r\n");
    }

    uprint("\r\nForth commands:\r\n");
    uprint("  HZ MS tone              -- pure tone\r\n");
    uprint("  F0 F1 MS sweep          -- linear sweep\r\n");
    uprint("  FC FM MS am             -- AM-modulated tone\r\n");
    uprint("  MS sleep                -- silence\r\n");
    uprint("  alert / notify / warn / error / sunrise\r\n");
    uprint("  siren / yelp / nee-naw / air-raid\r\n");
    uprint("  N vol  /  vol?          -- master volume 0..100\r\n");
    uprint("  audio-on / audio-off / audio-stop / audio-status\r\n");
    uprint("  prov-status / prov-reset / hive-status\r\n");
    uprint("  kv-get / kv-put\r\n\r\n");

    /* Sunrise boot chirp — plays once everything is up. The render task
     * is already alive; play_pattern just enqueues. */
    if (arc == 0) craw_audio_play_sunrise();

    forth_repl(console_getchar, console_putchar);
    forth_deinit();
}
