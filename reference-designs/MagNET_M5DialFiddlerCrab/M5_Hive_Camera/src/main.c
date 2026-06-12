/*
 * M5_Hive_Camera — OV2640 MagNET hive camera node.
 *
 * Targets:
 *   [env:esp32cam]  AI-Thinker ESP32-CAM (classic ESP32, UART0 console)
 *   [env:m5cams3]   M5Stack Unit CamS3    (ESP32-S3, native USB-serial-JTAG)
 *
 * Preserves the stock ESP32 CameraWebServer behavior on the HTTP side
 * (/stream, /capture, /control, /status) while adding MagNET provisioning
 * + hive join. Follows the post-WiFi bringup sequence that's been validated
 * on Echo/Dial/C3U: BLE teardown → SNTP sync → hive node start.
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "mdns.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  #include "driver/usb_serial_jtag.h"
#else
  #include "driver/uart.h"
#endif

#include "forth_core.h"
#include "forth_version.h"
#include "craw_nvs.h"
#include "craw_wifi.h"
#include "craw_ble_provision.h"
#include "craw_hive.h"
#include "craw_camera.h"
#include "../../include/magnet_gen.h"

/* http_stream.c */
int  cam_http_start(void);
void cam_http_stop(void);

static const char *TAG = "m5cam_hive";

/* Board selection at compile time. platformio.ini sets exactly one. */
#if defined(CAMERA_BOARD_M5CAMS3)
  #define BOARD_ENUM  CRAW_CAMERA_BOARD_M5CAMS3
  #define BOARD_SLUG  "cams3"
  /* M5CamS3 has a small single-color status LED on GPIO14 (per
   * components/craw_camera/pins_m5cams3.h). Drive it via LEDC PWM so
   * we can encode the UDM §10.9 four-state provisioning UX as
   * cadences (no RGB available — colors degrade to rhythm only). */
  #define HAS_STATUS_LED 1
  #include "driver/ledc.h"
  #define STATUS_LED_GPIO  14
#elif defined(CAMERA_BOARD_AI_THINKER)
  #define BOARD_ENUM  CRAW_CAMERA_BOARD_AI_THINKER
  #define BOARD_SLUG  "esp32cam"
  /* AI-Thinker's only addressable LED is the blinding white flash on
   * GPIO4 — not suitable for continuous status. Leave HAS_STATUS_LED
   * undefined; the status_led_* helpers compile to nothing. */
#elif defined(CAMERA_BOARD_M5CAMERAX)
  #define BOARD_ENUM  CRAW_CAMERA_BOARD_M5CAMERAX
  #define BOARD_SLUG  "m5camerax"
  /* M5Camera X has no status LED broken out (pins_m5camerax.h sets
   * LED_GPIO -1). Leave HAS_STATUS_LED undefined. */
#else
  #error "Define CAMERA_BOARD_AI_THINKER, CAMERA_BOARD_M5CAMS3, or CAMERA_BOARD_M5CAMERAX via platformio.ini"
#endif

#define FORTH_HEAP_SIZE (32 * 1024)
#define TIME_SYNC_EPOCH_THRESHOLD 1577836800  /* 2020-01-01 */

/* --------- Console I/O (UART0 for esp32cam, USB-JTAG for cams3) --------- */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static void console_init(void) {
    usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 512, .rx_buffer_size = 512 };
    usb_serial_jtag_driver_install(&cfg);
}
static void console_write(const char *s) {
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(500));
}
static int console_getchar(void) {
    uint8_t c;
    int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(10));
    return n > 0 ? c : -1;
}
static void console_putchar(int c) {
    uint8_t ch = (uint8_t)c;
    usb_serial_jtag_write_bytes(&ch, 1, pdMS_TO_TICKS(100));
}
#else
#define CONSOLE_UART UART_NUM_0
static void console_init(void) {
    uart_config_t cfg = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(CONSOLE_UART, 512, 512, 0, NULL, 0);
    uart_param_config(CONSOLE_UART, &cfg);
}
static void console_write(const char *s) {
    uart_write_bytes(CONSOLE_UART, s, strlen(s));
}
static int console_getchar(void) {
    uint8_t c;
    int n = uart_read_bytes(CONSOLE_UART, &c, 1, pdMS_TO_TICKS(10));
    return n > 0 ? c : -1;
}
static void console_putchar(int c) {
    uint8_t ch = (uint8_t)c;
    uart_write_bytes(CONSOLE_UART, &ch, 1);
}
#endif

static void uprint(const char *s) { console_write(s); }
static void uprintf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_write(buf);
}

/* -------------------- State --------------------- */
static char     ip_str[20]      = "N/A";
static char     hostname[40]    = "magnet-cam-0000";
static char     node_id[40]     = "MagNET-biologic-0000";
static bool     ble_teardown_requested = false;
static bool     ble_torn_down          = false;
static bool     sntp_started           = false;
static bool     time_synced            = false;
static bool     http_started           = false;
static bool     mdns_published         = false;
static bool     hive_started           = false;
static bool     camera_ready           = false;

/* -------------------- mDNS advertisement -------------------- */
static void mdns_publish(void) {
    if (mdns_published) return;
    mdns_init();
    mdns_hostname_set(hostname);                /* → <hostname>.local */
    mdns_instance_name_set("MagNET Hive Camera");
    /* TXT stream_port: MJPEG lives on its own httpd instance on :81
     * (http_stream.c) so a parked viewer can't wedge the :80 control plane.
     * Clients that only know _http find the stream via this key. */
    mdns_txt_item_t http_txt[] = {
        {"stream_port", "81"},
    };
    mdns_service_add(NULL, "_http", "_tcp", 80,
                     http_txt, sizeof(http_txt)/sizeof(http_txt[0]));
    /* Advertise the MagNET node type too so hive-aware clients can find it
     * without knowing the http service. TXT: role=spy, caps=camera+jpeg. */
    mdns_txt_item_t node_txt[] = {
        {"role", "spy"},
        {"caps", "camera,jpeg"},
        {"ver",  "1"},
    };
    mdns_service_add(NULL, "_magnet-node", "_tcp", 0,
                     node_txt, sizeof(node_txt)/sizeof(node_txt[0]));
    mdns_published = true;
    uprintf("[mDNS] %s.local  _http._tcp:80 (stream_port=81)\r\n", hostname);
}

/* -------------------- WiFi / BLE provisioning --------- */
/* ---- WiFi-state UI for the M5CamS3 status LED (single-color cadences) ----
 *
 * 0 = OFF / unconfigured  → slow blink ~1 Hz
 * 1 = CONNECTING          → fast blink ~3 Hz
 * 2 = CONNECTED           → smooth breath ~0.4 Hz
 * 3 = FAILED              → strobe ~8 Hz
 *
 * Maps the UDM §10.9 four-state provisioning UX vocabulary to PWM duty
 * (since the M5CamS3 has no RGB). status_led_task is a no-op build on
 * AI-Thinker (HAS_STATUS_LED undefined there). */
static volatile int s_wifi_state = 0;

#if HAS_STATUS_LED
static void status_led_init(void) {
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num   = STATUS_LED_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&c);
}

static inline void status_led_duty(uint8_t d) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, d);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

#define STATUS_LED_CAP  200   /* 8-bit duty; cap below 255 to be eye-friendly */

static void status_led_task(void *arg) {
    (void)arg;
    uint32_t f = 0;
    while (1) {
        uint8_t duty = 0;
        switch (s_wifi_state) {
        case 2: {  /* connected: breath ~0.4 Hz */
            float ph = (float)f * 0.033f * 6.2832f * 0.4f;
            float k  = 0.10f + 0.50f * (0.5f - 0.5f * cosf(ph));
            duty = (uint8_t)(k * STATUS_LED_CAP);
            break;
        }
        case 3:    /* failed: strobe 8 Hz */
            if ((f / 2) & 1) duty = STATUS_LED_CAP;
            break;
        case 1:    /* connecting: fast blink ~3 Hz */
            if ((f / 5) & 1) duty = STATUS_LED_CAP;
            break;
        default:   /* off / unconfigured: slow blink ~1 Hz */
            if ((f / 15) & 1) duty = STATUS_LED_CAP;
            break;
        }
        status_led_duty(duty);
        f++;
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
#endif /* HAS_STATUS_LED */

static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    (void)ctx;
    switch (event) {
    case CRAW_WIFI_EVENT_CONNECTED:
        s_wifi_state = 2;
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
        s_wifi_state = 0;
        uprint("\r\n[WiFi] disconnected\r\n");
        strncpy(ip_str, "N/A", sizeof(ip_str));
        if (!ble_torn_down) {
            craw_ble_provision_set_ip(ip_str);
            craw_ble_provision_advertise();
        }
        break;
    case CRAW_WIFI_EVENT_CONNECT_FAILED:
        s_wifi_state = 3;
        uprint("\r\n[WiFi] failed\r\n");
        strncpy(ip_str, "N/A", sizeof(ip_str));
        if (!ble_torn_down) {
            craw_ble_provision_set_ip(ip_str);
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
        s_wifi_state = 1;
        craw_wifi_connect(ssid, pass ? pass : "");
        break;
    default: break;
    }
}

static void on_hive_state(craw_hive_node_state_t state,
                          const char *info, void *ctx) {
    (void)ctx;
    uprintf("\r\n[HIVE] state=%d (%s)\r\n", (int)state, info ? info : "");
}

/* -------------------- Hive node start ---------------------- */
static const char *CAPS[] = { "camera", "jpeg", NULL };
static uint8_t     secret_copy[32];

static void derive_ids(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(hostname, sizeof(hostname), "magnet-cam-%02x%02x", mac[4], mac[5]);
    snprintf(node_id,  sizeof(node_id),  "MagNET-biologic-%02x%02x", mac[4], mac[5]);
}

static void maybe_start_hive(void) {
    if (hive_started) return;
    if (!craw_wifi_is_connected()) return;
    if (!time_synced) return;  /* gates on SNTP COMPLETED, not raw time() */
    memcpy(secret_copy, CRAW_HIVE_DEV_SECRET, 32);
    static craw_hive_node_config_t ncfg;
    ncfg = (craw_hive_node_config_t){
        .node_id        = node_id,
        .hive_id        = "beehive-1",
        .role_requested = "spy",
        .caps           = CAPS,
        .chip           = BOARD_SLUG,
        .fw             = "0.1.0",
        .gen            = MAGNET_GEN_STR,
        .secret         = secret_copy,
        .on_state       = on_hive_state,
        .on_state_ctx   = NULL,
    };
    if (craw_hive_node_start(&ncfg) == 0) {
        hive_started = true;
        uprint("[HIVE] node started as role=spy caps=camera,jpeg\r\n");
    }
}

/* -------------------- Housekeeping task --------------------- */
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
            /* Hard gate: wait for SNTP COMPLETED, not just time>2020. */
            sntp_sync_status_t st = sntp_get_sync_status();
            time_t now = time(NULL);
            if (st == SNTP_SYNC_STATUS_COMPLETED && now > TIME_SYNC_EPOCH_THRESHOLD) {
                time_synced = true;
                struct tm t;
                localtime_r(&now, &t);
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
                uprintf("[SNTP] time synced: %s UTC\r\n", buf);
            }
        }
        if (ble_torn_down && !camera_ready) {
            /* Now that the BLE stack is gone, internal RAM is unfragmented
             * and there's plenty of headroom for esp32-camera DMA.
             *
             * Default to VGA (640x480) rather than SVGA — this is the
             * stability sweet spot on AI-Thinker ESP32-CAM: bigger frames
             * cause PSRAM bandwidth saturation and correlate with WiFi-TX
             * brownouts that manifest as color-flash artifacts and dropped
             * frames. Users can override via `cam-framesize N` at the REPL
             * (0=QQVGA .. 10=UXGA) — the setting is persisted to NVS. */
            craw_camera_config_t ccfg = {
                .board        = (craw_camera_board_t)BOARD_ENUM,
                .xclk_freq_hz = 20000000,
                .jpeg_quality = 12,
                .frame_size   = FRAMESIZE_VGA,
                .fb_count     = 2,
                .pixel_format = PIXFORMAT_JPEG,
            };
            int rc = craw_camera_init(&ccfg);
            if (rc == 0) {
                /* Apply any persisted settings (framesize / quality / flip /
                 * mirror). Silently falls through if NVS is empty. */
                craw_camera_apply_saved_settings();
                camera_ready = true;
                uprintf("[cam] %s ready  (defaults VGA/q12, NVS overrides applied)  flash GPIO=%d\r\n",
                        craw_camera_board_name(), craw_camera_flash_gpio());
            } else {
                uprintf("[cam] init failed rc=0x%x — check wiring / PSRAM\r\n", rc);
            }
        }
        if (camera_ready && craw_wifi_is_connected() && !http_started) {
            /* Derive ids from MAC once WiFi is up so netif reports the
             * correct station MAC. Then bring up HTTP + mDNS. */
            derive_ids();
            cam_http_start();
            mdns_publish();
            http_started = true;
            uprintf("[HTTP] stream: http://%s.local/stream  or  http://%s/stream\r\n",
                    hostname, ip_str);
        }
        maybe_start_hive();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* -------------------- Forth FFI words ---------------------- */

/* ( -- size ) Capture one JPEG frame, print + push size to stack. */
static void w_cam_snap(void) {
    if (!camera_ready) {
        uprint("\r\ncam-snap: camera not ready (waits for WiFi up + BLE teardown)\r\n");
        forth_push(0);
        return;
    }
    camera_fb_t *fb = craw_camera_capture();
    if (!fb) {
        uprint("\r\ncam-snap: FAILED\r\n");
        forth_push(0);
        return;
    }
    uprintf("\r\nJPEG %u bytes  (%ux%u)\r\n",
            (unsigned)fb->len, (unsigned)fb->width, (unsigned)fb->height);
    int n = (int)fb->len;
    craw_camera_release(fb);
    forth_push(n);
}

/* ( q -- ) Set JPEG quality 0-63 (lower = better). */
static void w_cam_quality(void) {
    int q = (int)forth_pop();
    int rc = craw_camera_set_quality(q);
    uprintf("\r\ncam-quality=%d rc=%d\r\n", q, rc);
}

/* ( fs -- ) Set frame size 0=QQVGA .. 10=UXGA. */
static void w_cam_framesize(void) {
    int fs = (int)forth_pop();
    int rc = craw_camera_set_framesize(fs);
    uprintf("\r\ncam-framesize=%d rc=%d\r\n", fs, rc);
}

/* ( n -- ) Vertical flip (upside-down): 1 = on, 0 = off. */
static void w_cam_vflip(void) {
    int n = (int)forth_pop();
    int rc = craw_camera_set_vflip(n);
    uprintf("\r\ncam-vflip=%d rc=%d\r\n", n ? 1 : 0, rc);
}

/* ( n -- ) Horizontal mirror: 1 = on, 0 = off. */
static void w_cam_hmirror(void) {
    int n = (int)forth_pop();
    int rc = craw_camera_set_hmirror(n);
    uprintf("\r\ncam-hmirror=%d rc=%d\r\n", n ? 1 : 0, rc);
}

static void w_flash_on(void)  { craw_camera_flash_on();  uprint("\r\nflash on\r\n"); }
static void w_flash_off(void) { craw_camera_flash_off(); uprint("\r\nflash off\r\n"); }

/* ( -- ) Clear persisted camera settings (framesize / quality / flips / xclk).
 * Defaults resume on next boot. */
static void w_cam_reset_settings(void) {
    craw_camera_clear_saved_settings();
    uprint("\r\nCamera settings cleared from NVS (takes effect on reboot).\r\n");
}

/* ( mhz -- ) Override XCLK frequency. Typical values: 20 (default / fastest,
 * OV2640 rated), 16, 10 (stability fallback), 8 (ultra-conservative).
 * Persisted — takes effect on next boot (XCLK is locked at camera init). */
static void w_cam_xclk_mhz(void) {
    int mhz = (int)forth_pop();
    craw_camera_set_xclk_mhz(mhz);
    uprintf("\r\ncam-xclk-mhz=%d saved. Reboot to apply.\r\n", mhz);
}

static void w_stream_url(void) {
    if (!http_started) { uprint("\r\n(no WiFi yet)\r\n"); return; }
    /* MJPEG stream is on :81 (own httpd instance — see http_stream.c). */
    uprintf("\r\nhttp://%s.local:81/stream\r\nhttp://%s:81/stream\r\n", hostname, ip_str);
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
    uprintf("board:  %s\r\n", craw_camera_board_name());
    uprintf("camera: %s\r\n", camera_ready ? "ready" : "pending (needs WiFi + BLE-torn-down)");
    uprintf("hive:   %s\r\n", hive_started ? "joined" : "idle");
}

static void w_prov_reset(void) {
    char ssid[33], pass[65];
    craw_nvs_clear_wifi_creds(ssid, pass);
    uprint("\r\nCreds cleared. Rebooting to re-enter provisioning...\r\n");
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

/* ---- KV REPL helpers (Milestone C step 1) ----
 * Camera RAM is tight (esp32-camera DMA + BLE + WiFi). Cap KV values at
 * 1 KB for REPL use; Camera doesn't need full 3 KB values for v1. Bundle
 * fetches (Step 2+) will allocate from heap on demand. */
#define CAMERA_KV_VAL_MAX  1024
static char s_kv_key[CRAW_HIVE_KV_KEY_MAX + 1];
static char s_kv_val[CAMERA_KV_VAL_MAX + 1];

static void kv_read_line(const char *prompt, char *buf, size_t bufsz) {
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

/* ( -- ) Send KV_GET to the ruler, print response. */
static void w_kv_get(void) {
    kv_read_line("\r\nkey: ", s_kv_key, sizeof(s_kv_key));
    int rc = craw_hive_node_kv_get(s_kv_key, s_kv_val, sizeof(s_kv_val), 3000);
    if (rc == 0)       uprintf("'%s' = '%s'\r\n", s_kv_key, s_kv_val);
    else if (rc == 1)  uprint("not found\r\n");
    else if (rc == -2) uprint("timeout\r\n");
    else               uprintf("kv-get failed rc=%d\r\n", rc);
}

/* ( -- ) Send KV_PUT (fire-and-forget). */
static void w_kv_put(void) {
    kv_read_line("\r\nkey:   ", s_kv_key, sizeof(s_kv_key));
    kv_read_line("value: ",     s_kv_val, sizeof(s_kv_val));
    int rc = craw_hive_node_kv_put(s_kv_key, s_kv_val);
    if (rc == 0) uprintf("sent KV_PUT '%s'\r\n", s_kv_key);
    else         uprintf("kv-put failed rc=%d\r\n", rc);
}

static void register_forth_words(void) {
    forth_register_word("cam-snap",      w_cam_snap);
    forth_register_word("cam-quality",   w_cam_quality);
    forth_register_word("cam-framesize", w_cam_framesize);
    forth_register_word("cam-vflip",     w_cam_vflip);
    forth_register_word("cam-hmirror",   w_cam_hmirror);
    forth_register_word("cam-reset-settings", w_cam_reset_settings);
    forth_register_word("cam-xclk-mhz",       w_cam_xclk_mhz);
    forth_register_word("kv-get",             w_kv_get);
    forth_register_word("kv-put",             w_kv_put);
    forth_register_word("flash-on",      w_flash_on);
    forth_register_word("flash-off",     w_flash_off);
    forth_register_word("stream-url",    w_stream_url);
    forth_register_word("prov-status",   w_prov_status);
    forth_register_word("prov-reset",    w_prov_reset);
    forth_register_word("hive-status",   w_hive_status);
}

/* -------------------- app_main ----------------------------- */
void app_main(void) {
    console_init();
    vTaskDelay(pdMS_TO_TICKS(500));

    uprint("\r\n\r\n====================================\r\n");
    uprintf( "  M5 Hive Camera (board=%s)\r\n", BOARD_SLUG);
    uprintf( "  ESPIDFORTH %s\r\n", ESPIDFORTH_VERSION_STRING);
    uprint(  "  Preserves stock /stream /capture /control\r\n");
    uprint(  "  + BLE provisioning + hive join (role=spy)\r\n");
    uprint(  "====================================\r\n");

    /* Camera init is DEFERRED to housekeeping_task, after BLE has been torn
     * down. Running the esp32-camera DMA + driver concurrent with the BLE
     * controller on classic ESP32 causes BT_INIT malloc failures when a
     * BLE client connects — the camera driver takes the prime contiguous
     * internal RAM and the BT controller runs out at connection time.
     * Camera is useless pre-WiFi anyway, so the staged pattern is cleaner. */
    uprintf("[cam] init deferred until WiFi is up (board=%s)\r\n", BOARD_SLUG);

    /* NVS + WiFi + BLE provisioning (same post-WiFi order as Echo). */
    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    craw_wifi_init("MagNET-biologic", on_wifi_event, NULL);

    craw_ble_provision_config_t pcfg = {
        .name_prefix = "MagNET-biologic",
        .role        = "spy",
    };
    craw_ble_provision_init(&pcfg, on_prov_event, NULL);
    uprintf("BLE: %s\r\n", craw_ble_provision_device_name());

    /* Forth */
    forth_init(FORTH_HEAP_SIZE);
    register_forth_words();
    uprintf("Free heap: %lu bytes\r\n", (unsigned long)esp_get_free_heap_size());

    xTaskCreate(housekeeping_task, "keep", 6144, NULL, 3, NULL);

#if HAS_STATUS_LED
    status_led_init();
    xTaskCreate(status_led_task, "stled", 2048, NULL, 2, NULL);
    uprintf("Status LED on GPIO %d (PWM cadence: WiFi state)\r\n", STATUS_LED_GPIO);
#endif

    /* Auto-connect on reboot if creds exist */
    char ssid[33], pass[65];
    if (craw_nvs_load_wifi_creds(ssid, pass) && ssid[0]) {
        uprintf("Stored WiFi '%s' — auto-connect\r\n", ssid);
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
        s_wifi_state = 1;
        craw_wifi_connect(ssid, pass);
    } else {
        uprint("No stored WiFi — provision via BLE.\r\n");
    }

    uprint("\r\nForth commands:\r\n");
    uprint("  cam-snap / cam-quality / cam-framesize\r\n");
    uprint("  flash-on / flash-off / stream-url\r\n");
    uprint("  prov-status / prov-reset / hive-status\r\n\r\n");

    forth_repl(console_getchar, console_putchar);
    forth_deinit();
}
