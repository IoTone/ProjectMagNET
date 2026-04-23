/*
 * M5Atom_Echo_Hex_Hive_Test — MagNET hive-protocol bringup with display + sound.
 *
 * Base: M5Atom Echo (ESP32-PICO-D4). Onboard I2S speaker (NS4168 amp) for
 * audible state-transition chirps. Onboard button on GPIO 39.
 * Peripheral: M5 Unit Hex — 37 × SK6812 LEDs on Grove GPIO 26, hex-packed
 * (1 center + 6 + 12 + 18 rings). Used as the hive-state status panel.
 *
 * Note on hex LED indexing: the physical wiring order of the 37 LEDs is
 * device-dependent and not clearly documented in the Unit Hex wiki. This
 * firmware assumes index 0 is the center and works outward, but the actual
 * correspondence must be mapped on hardware with the `hex-test` Forth word
 * (lights each LED in turn, 300 ms apart). Adjust the HEX_*_IDX constants
 * below after observation if needed.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2s_std.h"
#include "led_strip.h"

#include <time.h>

#include "forth_core.h"
#include "forth_version.h"

#include "craw_nvs.h"
#include "craw_wifi.h"
#include "craw_ble_provision.h"
#include "craw_hive.h"

static const char *TAG = "m5echo_hive";

/* ---- Pin map (M5Atom Echo + M5 Unit Hex on Grove) ---- */
#define HEX_LED_GPIO        26          /* Grove port DATA pin on Atom Echo */
#define HEX_LED_COUNT       37
#define BTN_GPIO            39          /* Onboard button (input-only, external PU) */
#define CONSOLE_UART        UART_NUM_0
#define CONSOLE_BAUD        115200

/* Atom Echo NS4168 speaker I2S wiring per M5 schematic */
#define I2S_BCLK_GPIO       19
#define I2S_WS_GPIO         33
#define I2S_DOUT_GPIO       22
#define I2S_SAMPLE_RATE     16000

/* Logical status slots on the hex. Provisional — confirm with `hex-test`. */
#define HEX_IDX_HIVE        0           /* center */
#define HEX_IDX_BLE         1           /* inner-ring 0 */
#define HEX_IDX_WIFI        2           /* inner-ring 1 */
#define HEX_IDX_HEARTBEAT   3           /* inner-ring 2 */
/* Middle / outer rings reserved for role / caps in a later milestone. */

#define FORTH_HEAP_SIZE     (32 * 1024)

/* ---- LED strip ---- */
static led_strip_handle_t s_hex = NULL;

static void hex_clear(void) { if (s_hex) led_strip_clear(s_hex); }
static void hex_set(int i, uint8_t r, uint8_t g, uint8_t b) {
    if (!s_hex || i < 0 || i >= HEX_LED_COUNT) return;
    led_strip_set_pixel(s_hex, i, r, g, b);
}
static void hex_refresh(void) { if (s_hex) led_strip_refresh(s_hex); }

static void hex_init(void) {
    led_strip_config_t cfg = {
        .strip_gpio_num = HEX_LED_GPIO,
        .max_leds       = HEX_LED_COUNT,
    };
    led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &s_hex));
    hex_clear();
    hex_refresh();
}

/* ---- I2S tone player ---- */
static i2s_chan_handle_t s_tx_chan = NULL;

typedef struct {
    uint16_t freq_hz;      /* 0 = silent gap */
    uint16_t duration_ms;
} tone_cmd_t;
static QueueHandle_t s_tone_q;

static void i2s_init(void) {
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* 8 × 512 frames = 4096 frames ≈ 256 ms of DMA buffer. Gives the tone
     * task substantial runway to feed silence even if preempted by WiFi /
     * NimBLE / TCP work. Costs 16 KB internal RAM — acceptable on classic
     * ESP32 where we still have ~200 KB free. */
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 512;
    chan_cfg.auto_clear    = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        /* NS4168 reads both L and R channels — mono produces silence. */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    /* Channel stays enabled for the life of the process. The tone task
     * continuously writes either sine samples or silence, which keeps the
     * NS4168 fed with valid I2S data so the amp outputs silence (not noise)
     * between tones. Disabling the channel leaves the amp hot but unfed,
     * which produces a continuous hiss/hum until the next tone. */
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
}

static void play_tone_blocking(uint16_t freq_hz, uint16_t duration_ms) {
    if (!s_tx_chan || duration_ms == 0) return;
    const int sample_rate = I2S_SAMPLE_RATE;
    int total_frames = (int)(sample_rate * (uint32_t)duration_ms / 1000);
    /* Stereo: each frame = 2× int16_t (L, R). Buffer is 128 frames = 256 int16_t. */
    int16_t buf[256];
    double phase = 0.0;
    double phase_inc = freq_hz ? (2.0 * M_PI * (double)freq_hz / sample_rate) : 0.0;
    size_t written = 0;
    while (total_frames > 0) {
        int frames = total_frames > 128 ? 128 : total_frames;
        if (freq_hz == 0) {
            memset(buf, 0, frames * 2 * sizeof(int16_t));
        } else {
            for (int i = 0; i < frames; i++) {
                int16_t s = (int16_t)(sin(phase) * 12000.0);
                buf[i * 2]     = s;  /* L */
                buf[i * 2 + 1] = s;  /* R */
                phase += phase_inc;
                if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            }
        }
        i2s_channel_write(s_tx_chan, buf, frames * 2 * sizeof(int16_t),
                          &written, portMAX_DELAY);
        total_frames -= frames;
    }
}

static void tone_task(void *arg) {
    (void)arg;
    /* Continuously write either queued tone data or silence. This keeps the
     * NS4168 fed with valid samples so it outputs true silence between
     * tones — without a constant stream, the amp hisses. */
    static int16_t silence[256] = {0};
    tone_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_tone_q, &cmd, 0) == pdTRUE) {
            play_tone_blocking(cmd.freq_hz, cmd.duration_ms);
        } else {
            /* 128-frame stereo chunk of silence ≈ 8 ms @ 16 kHz. Blocks
             * until the DMA consumes it, so CPU load is bounded by the
             * sample rate, not the scheduler. */
            size_t written = 0;
            i2s_channel_write(s_tx_chan, silence, sizeof(silence),
                              &written, portMAX_DELAY);
        }
    }
}

/* Fire-and-forget — safe to call from any context. */
static void beep(uint16_t freq_hz, uint16_t duration_ms) {
    tone_cmd_t cmd = { .freq_hz = freq_hz, .duration_ms = duration_ms };
    xQueueSend(s_tone_q, &cmd, 0);
}

/* Multi-tone patterns for state transitions. */
static void tune_ble_advert(void)   { beep(1000, 80); }
static void tune_wifi_up(void)      { beep(800, 80); beep(0, 40); beep(1200, 90); }
static void tune_wifi_fail(void)    { beep(800, 80); beep(0, 40); beep(500, 120); }
static void tune_hive_joined(void)  { beep(1500, 80); beep(0, 40); beep(2000, 100); }
static void tune_hive_backoff(void) { beep(700, 60); beep(0, 30); beep(500, 60); }
static void tune_heartbeat(void)    { beep(400, 20); }

/* ---- Status model ---- */
typedef enum { BLE_OFF, BLE_ADVERT, BLE_CONNECTED } ble_ui_t;
typedef enum { WIFI_OFF, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED } wifi_ui_t;

static volatile ble_ui_t  s_ble_ui  = BLE_OFF;
static volatile wifi_ui_t s_wifi_ui = WIFI_OFF;
static volatile craw_hive_node_state_t s_hive_ui = CRAW_HIVE_NODE_OFFLINE;
static volatile int64_t s_heartbeat_until_ms = 0;
static char s_ip_str[20] = "N/A";
/* Deferred BLE teardown: set from the WiFi event callback, acted on by
 * housekeeping_task so deinit runs outside any NimBLE callback context.
 * Once true, BLE is gone until reboot (see prov-reset). */
static volatile bool s_ble_teardown_requested = false;
static volatile bool s_ble_torn_down          = false;

/* SNTP must complete before the hive node can speak the protocol — HMAC
 * payloads include a unix-timestamp field and the ruler rejects anything
 * more than 30 s off. Without SNTP the ESP32 thinks time() ≈ boot uptime. */
static bool s_sntp_started = false;
static bool s_time_synced  = false;
#define TIME_SYNC_EPOCH_THRESHOLD 1577836800 /* 2020-01-01 — anything > this is real time */

/* Hash of last-painted state. Refresh the physical LEDs only when any of
 * ble/wifi/hive status or the heartbeat-flash window has changed; otherwise
 * leave the strip untouched. Reduces WS2812 data-line toggling (and its
 * associated current transients that couple into the NS4168 amp) from a
 * constant 10 Hz to effectively zero when idle. */
static void paint_status(void) {
    if (!s_hex) return;
    static ble_ui_t  last_ble  = (ble_ui_t)-1;
    static wifi_ui_t last_wifi = (wifi_ui_t)-1;
    static craw_hive_node_state_t last_hive = (craw_hive_node_state_t)-1;
    static bool last_heartbeat_on = false;

    int64_t now_ms = esp_timer_get_time() / 1000;
    bool heartbeat_on = now_ms < s_heartbeat_until_ms;

    if (last_ble == s_ble_ui && last_wifi == s_wifi_ui &&
        last_hive == s_hive_ui && last_heartbeat_on == heartbeat_on) {
        return; /* nothing changed — don't refresh the strip */
    }
    last_ble = s_ble_ui;
    last_wifi = s_wifi_ui;
    last_hive = s_hive_ui;
    last_heartbeat_on = heartbeat_on;

    hex_clear();
    switch (s_hive_ui) {
        case CRAW_HIVE_NODE_DISCOVER:   hex_set(HEX_IDX_HIVE, 0, 0, 80);  break;
        case CRAW_HIVE_NODE_CONNECTING: hex_set(HEX_IDX_HIVE, 80, 50, 0); break;
        case CRAW_HIVE_NODE_JOINED:     hex_set(HEX_IDX_HIVE, 0, 80, 0);  break;
        case CRAW_HIVE_NODE_BACKOFF:    hex_set(HEX_IDX_HIVE, 80, 0, 0);  break;
        default: break;
    }
    switch (s_ble_ui) {
        case BLE_ADVERT:    hex_set(HEX_IDX_BLE, 0, 80, 80);  break;
        case BLE_CONNECTED: hex_set(HEX_IDX_BLE, 80, 80, 80); break;
        default: break;
    }
    switch (s_wifi_ui) {
        case WIFI_CONNECTING: hex_set(HEX_IDX_WIFI, 80, 50, 0); break;
        case WIFI_CONNECTED:  hex_set(HEX_IDX_WIFI, 0, 80, 0);  break;
        case WIFI_FAILED:     hex_set(HEX_IDX_WIFI, 80, 0, 0);  break;
        default: break;
    }
    if (heartbeat_on) hex_set(HEX_IDX_HEARTBEAT, 80, 80, 80);
    hex_refresh();
}

static void ui_task(void *arg) {
    (void)arg;
    while (1) {
        paint_status();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ---- UART console ---- */
static void console_init(void) {
    uart_config_t cfg = {
        .baud_rate  = CONSOLE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(CONSOLE_UART, 512, 512, 0, NULL, 0);
    uart_param_config(CONSOLE_UART, &cfg);
}

static void uart_print(const char *s) { uart_write_bytes(CONSOLE_UART, s, strlen(s)); }
static void uart_printf(const char *fmt, ...) {
    char buf[256]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    uart_print(buf);
}
static int  console_getchar(void) {
    uint8_t c; int n = uart_read_bytes(CONSOLE_UART, &c, 1, pdMS_TO_TICKS(10));
    return (n <= 0) ? -1 : c;
}
static void console_putchar(int c) {
    uint8_t ch = (uint8_t)c; uart_write_bytes(CONSOLE_UART, &ch, 1);
}

/* ---- Button ---- */
static volatile int s_btn_released = 0;
static void IRAM_ATTR btn_isr(void *arg) {
    if (gpio_get_level(BTN_GPIO) == 1) s_btn_released = 1;
}
static void btn_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE, /* GPIO 39 input-only; external PU on board */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BTN_GPIO, btn_isr, NULL);
}

/* ---- WiFi + BLE provisioning callbacks ---- */
static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    (void)ctx;
    switch (event) {
        case CRAW_WIFI_EVENT_CONNECTED:
            craw_wifi_get_ip_str(s_ip_str, sizeof(s_ip_str));
            s_wifi_ui = WIFI_CONNECTED;
            uart_printf("\r\n[WiFi] connected, IP: %s\r\n", s_ip_str);
            craw_ble_provision_set_ip(s_ip_str);
            craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTED);
            if (!s_sntp_started) {
                esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
                esp_netif_sntp_init(&sntp_cfg);
                s_sntp_started = true;
                uart_print("[SNTP] sync kicked off (pool.ntp.org)\r\n");
            }
            /* BLE beacons couple into the NS4168 amp and cause periodic
             * clicks, and the BLE stack holds ~50 KB of RAM that mDNS / the
             * hive TCP task need. Tear it down entirely — the housekeeping
             * task will deinit NimBLE + the BT controller outside any
             * callback context. BLE won't come back until a reboot, which
             * is what prov-reset now does. */
            craw_ble_provision_stop_advertising();
            s_ble_ui = BLE_OFF;
            s_ble_teardown_requested = true;
            tune_wifi_up();
            break;
        case CRAW_WIFI_EVENT_DISCONNECTED:
            s_wifi_ui = WIFI_OFF;
            uart_print("\r\n[WiFi] disconnected\r\n");
            strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
            if (!s_ble_torn_down) {
                craw_ble_provision_set_ip(s_ip_str);
                craw_ble_provision_advertise();
                s_ble_ui = BLE_ADVERT;
            }
            break;
        case CRAW_WIFI_EVENT_CONNECT_FAILED:
            s_wifi_ui = WIFI_FAILED;
            uart_print("\r\n[WiFi] failed\r\n");
            strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
            if (!s_ble_torn_down) {
                craw_ble_provision_set_ip(s_ip_str);
                craw_ble_provision_set_status(CRAW_BLE_PROV_FAILED);
                craw_ble_provision_advertise();
                s_ble_ui = BLE_ADVERT;
            }
            tune_wifi_fail();
            break;
    }
}

static void on_prov_event(craw_ble_prov_state_t state,
                          const char *ssid, const char *pass, void *ctx) {
    (void)ctx;
    switch (state) {
        case CRAW_BLE_PROV_CREDS_RECEIVED:
            if (s_ble_ui != BLE_CONNECTED) {
                s_ble_ui = BLE_CONNECTED;
                tune_ble_advert();
            }
            uart_printf("\r\n[PROV] creds: ssid='%s'\r\n", ssid);
            break;
        case CRAW_BLE_PROV_COMMIT_REQUESTED:
            if (!ssid || !ssid[0]) break;
            uart_printf("\r\n[PROV] commit -> '%s'\r\n", ssid);
            craw_nvs_save_wifi_creds(ssid, pass ? pass : "");
            s_wifi_ui = WIFI_CONNECTING;
            craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
            craw_wifi_connect(ssid, pass ? pass : "");
            break;
        default:
            break;
    }
}

/* ---- Hive node callback ---- */
static void on_hive_state(craw_hive_node_state_t state,
                          const char *info, void *ctx) {
    (void)ctx;
    craw_hive_node_state_t prev = s_hive_ui;
    s_hive_ui = state;
    uart_printf("\r\n[HIVE] state=%d (%s)\r\n", (int)state, info ? info : "");
    if (state == CRAW_HIVE_NODE_JOINED && prev != CRAW_HIVE_NODE_JOINED) tune_hive_joined();
    if (state == CRAW_HIVE_NODE_BACKOFF && prev == CRAW_HIVE_NODE_JOINED) tune_hive_backoff();
    s_heartbeat_until_ms = (esp_timer_get_time() / 1000) + 200;
}

/* ---- Hive start (deferred until WiFi is up) ---- */
static bool s_hive_started = false;
static const char *s_caps[] = { "led", "hex", "button", "speaker", NULL };
static char s_node_id[40] = {0};

static void derive_node_id(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_node_id, sizeof(s_node_id), "MagNET-biologic-%02x%02x", mac[4], mac[5]);
}

static void maybe_start_hive(void) {
    if (s_hive_started) return;
    if (!craw_wifi_is_connected()) return;
    /* Block hive start until the system clock is past 2020 — before SNTP
     * completes, time() returns seconds since boot, and the HMAC timestamp
     * would be off by billions of seconds. */
    if (time(NULL) < TIME_SYNC_EPOCH_THRESHOLD) return;
    derive_node_id();
    static craw_hive_node_config_t ncfg;
    ncfg = (craw_hive_node_config_t){
        .node_id        = s_node_id,
        .hive_id        = "beehive-1",
        .role_requested = "spawn",
        .caps           = s_caps,
        .chip           = "ESP32",
        .fw             = "0.1.0",
        .secret         = (const uint8_t *)CRAW_HIVE_DEV_SECRET,
        .on_state       = on_hive_state,
        .on_state_ctx   = NULL,
    };
    if (craw_hive_node_start(&ncfg) == 0) {
        s_hive_started = true;
        uart_print("[HIVE] node started\r\n");
    }
}

static void housekeeping_task(void *arg) {
    (void)arg;
    while (1) {
        if (s_btn_released) {
            s_btn_released = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BTN_GPIO) == 1) {
                s_heartbeat_until_ms = (esp_timer_get_time() / 1000) + 400;
                tune_heartbeat();
            }
        }
        if (s_ble_teardown_requested && !s_ble_torn_down) {
            /* Wait a beat so any in-flight notify on ip/status has drained. */
            vTaskDelay(pdMS_TO_TICKS(500));
            size_t before = esp_get_free_heap_size();
            craw_ble_provision_deinit();
            s_ble_torn_down = true;
            size_t after  = esp_get_free_heap_size();
            uart_printf("[BLE] torn down. Heap: %u -> %u (+%d bytes)\r\n",
                        (unsigned)before, (unsigned)after,
                        (int)after - (int)before);
        }
        if (s_sntp_started && !s_time_synced) {
            time_t now = time(NULL);
            if (now > TIME_SYNC_EPOCH_THRESHOLD) {
                s_time_synced = true;
                struct tm t;
                localtime_r(&now, &t);
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
                uart_printf("[SNTP] time synced: %s UTC\r\n", buf);
            }
        }
        maybe_start_hive();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* ---- Forth words ---- */
static void w_prov_status(void) {
    char ssid[33], pass[65];
    bool has = craw_nvs_load_wifi_creds(ssid, pass);
    uart_printf("\r\nble:   %s\r\n", craw_ble_provision_device_name());
    uart_printf("wifi:  %s\r\n", craw_wifi_is_connected() ? "connected" : "down");
    uart_printf("ssid:  %s\r\n", has ? ssid : "(none)");
    uart_printf("ip:    %s\r\n", s_ip_str);
}

static void w_prov_reset(void) {
    char ssid[33], pass[65];
    craw_nvs_clear_wifi_creds(ssid, pass);
    /* BLE stack has been torn down once WiFi connected — the only reliable
     * way to resume advertising is a reboot. NVS creds are already cleared,
     * so the node will come back up in provisioning mode. */
    uart_print("\r\nCreds cleared. Rebooting to re-enter provisioning...\r\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void w_hive_status(void) {
    const char *labels[] = { "OFFLINE","DISCOVER","CONNECTING","JOINED","BACKOFF" };
    int st = (int)craw_hive_node_state();
    if (st < 0 || st > 4) st = 0;
    const char *sid = craw_hive_node_session_id();
    uart_printf("\r\nhive:    %s\r\n", labels[st]);
    uart_printf("node:    %s\r\n", s_node_id[0] ? s_node_id : "(unset)");
    uart_printf("session: %s\r\n", sid ? sid : "(none)");
}

/* ( -- ) Walk every LED index, lighting one at a time for 300 ms. Use to
 * map physical hex positions → logical indices. Restores status when done. */
static void w_hex_test(void) {
    if (!s_hex) return;
    for (int i = 0; i < HEX_LED_COUNT; i++) {
        hex_clear();
        hex_set(i, 60, 60, 60);
        hex_refresh();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    paint_status();
}

/* ( idx r g b -- ) Set a single hex LED; next status repaint may overwrite. */
static void w_hex_pixel(void) {
    int b = (int)forth_pop();
    int g = (int)forth_pop();
    int r = (int)forth_pop();
    int i = (int)forth_pop();
    hex_set(i, (uint8_t)r, (uint8_t)g, (uint8_t)b);
    hex_refresh();
}

/* ( freq_hz duration_ms -- ) Play a single tone. */
static void w_beep(void) {
    int dur = (int)forth_pop();
    int freq = (int)forth_pop();
    if (freq < 0) freq = 0;
    if (dur < 0) dur = 0;
    if (dur > 2000) dur = 2000;
    beep((uint16_t)freq, (uint16_t)dur);
}

/* ( -- ) Stop BLE advertising (diagnostic: test whether amp clicks are
 * caused by BLE beacon RF coupling). Use `prov-reset` to resume. */
static void w_ble_stop(void) {
    craw_ble_provision_stop_advertising();
    s_ble_ui = BLE_OFF;
    uart_print("\r\nBLE advertising stopped.\r\n");
}

static void register_forth_words(void) {
    forth_register_word("prov-status", w_prov_status);
    forth_register_word("prov-reset",  w_prov_reset);
    forth_register_word("hive-status", w_hive_status);
    forth_register_word("hex-test",    w_hex_test);
    forth_register_word("hex-pixel",   w_hex_pixel);
    forth_register_word("beep",        w_beep);
    forth_register_word("ble-stop",    w_ble_stop);
}

/* ---- app_main ---- */
void app_main(void) {
    console_init();
    vTaskDelay(pdMS_TO_TICKS(500));

    uart_print("\r\n\r\n====================================\r\n");
    uart_print(  "  M5Atom Echo + Unit Hex Hive Test\r\n");
    uart_printf( "  ESPIDFORTH %s\r\n", ESPIDFORTH_VERSION_STRING);
    uart_print(  "  ESP32 | 37 hex LEDs | NS4168 speaker\r\n");
    uart_print(  "====================================\r\n");

    hex_init();
    btn_init();
    s_tone_q = xQueueCreate(8, sizeof(tone_cmd_t));
    i2s_init();
    /* Priority 10: well above UI/housekeeping but still below WiFi/BLE host
     * tasks. Ensures silence keeps flowing between tones without starving
     * the radio stacks. */
    xTaskCreate(tone_task, "tone", 4096, NULL, 10, NULL);
    uart_printf("Hex LEDs on GPIO %d (%d px), button on GPIO %d\r\n",
                HEX_LED_GPIO, HEX_LED_COUNT, BTN_GPIO);
    uart_printf("I2S speaker: BCLK=%d WS=%d DOUT=%d @ %d Hz\r\n",
                I2S_BCLK_GPIO, I2S_WS_GPIO, I2S_DOUT_GPIO, I2S_SAMPLE_RATE);

    /* Boot chirp — sanity check for speaker + I2S init. */
    beep(1000, 60);
    beep(0, 30);
    beep(1500, 60);

    /* NVS + WiFi + BLE provisioning */
    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    craw_wifi_init("MagNET-biologic", on_wifi_event, NULL);

    craw_ble_provision_config_t pcfg = {
        .name_prefix = "MagNET-biologic",
        .role        = "spawn",
    };
    craw_ble_provision_init(&pcfg, on_prov_event, NULL);
    s_ble_ui = BLE_ADVERT;
    tune_ble_advert();
    uart_printf("BLE: %s\r\n", craw_ble_provision_device_name());

    /* Forth */
    forth_init(FORTH_HEAP_SIZE);
    register_forth_words();
    uart_printf("Forth ready. Free heap: %lu bytes\r\n",
                (unsigned long)esp_get_free_heap_size());

    xTaskCreate(ui_task,           "ui",   3072, NULL, 2, NULL);
    xTaskCreate(housekeeping_task, "keep", 4096, NULL, 3, NULL);

    /* Auto-connect if creds are stored */
    char ssid[33], pass[65];
    if (craw_nvs_load_wifi_creds(ssid, pass) && ssid[0]) {
        uart_printf("Stored WiFi '%s' — auto-connect\r\n", ssid);
        s_wifi_ui = WIFI_CONNECTING;
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
        craw_wifi_connect(ssid, pass);
    } else {
        uart_print("No stored WiFi — provision via BLE.\r\n");
    }

    uart_print("\r\nForth commands:\r\n");
    uart_print("  prov-status / prov-reset / hive-status\r\n");
    uart_print("  hex-test                -- walk every LED to map positions\r\n");
    uart_print("  IDX R G B hex-pixel     -- set one LED\r\n");
    uart_print("  FREQ DUR_MS beep        -- play a tone\r\n\r\n");

    forth_repl(console_getchar, console_putchar);
    forth_deinit();
}
