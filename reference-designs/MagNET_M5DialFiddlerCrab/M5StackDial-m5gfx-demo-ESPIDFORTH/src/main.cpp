// M5Dial Playground Demo — ESP-IDF + ESPIDFORTH port
// Ported from Arduino/M5Unified to ESP-IDF with M5GFX direct driver,
// PCNT rotary encoder, LEDC buzzer, and Forth REPL integration.

#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_GC9A01.hpp>
#include <lgfx/v1/touch/Touch_FT5x06.hpp>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/usb_serial_jtag.h"
#include "forth_core.h"
#include "forth_version.h"

extern "C" {
#include "craw_nvs.h"
#include "craw_wifi.h"
#include "craw_ble_provision.h"
#include "craw_hive.h"
#include "bundle_bootstrap.h"
}
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>

static const char *TAG = "m5dial";

// ---------------------------------------------------------------------------
// millis() / delay() replacements
// ---------------------------------------------------------------------------
static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------------------
// USB Serial I/O (same pattern as Blinky E4TH)
// ---------------------------------------------------------------------------
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
    if (n <= 0) return -1;
    return c;
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

// ---------------------------------------------------------------------------
// LGFX display driver for M5Dial (GC9A01 240x240 round)
// ---------------------------------------------------------------------------
class LGFX_M5Dial : public lgfx::LGFX_Device {
    lgfx::Panel_GC9A01 _panel_instance;
    lgfx::Bus_SPI       _bus_instance;
    lgfx::Light_PWM     _light_instance;
    lgfx::Touch_FT5x06  _touch_instance;
public:
    LGFX_M5Dial(void) {
        // SPI bus: MOSI=5, SCLK=6, DC=4
        auto bus_cfg        = _bus_instance.config();
        bus_cfg.spi_host    = SPI2_HOST;
        bus_cfg.freq_write  = 80000000;
        bus_cfg.pin_mosi    = 5;
        bus_cfg.pin_sclk    = 6;
        bus_cfg.pin_dc      = 4;
        _bus_instance.config(bus_cfg);
        _panel_instance.setBus(&_bus_instance);

        // Panel: CS=7, RST=8, 240x240
        auto panel_cfg          = _panel_instance.config();
        panel_cfg.pin_cs        = 7;
        panel_cfg.pin_rst       = 8;
        panel_cfg.panel_width   = 240;
        panel_cfg.panel_height  = 240;
        panel_cfg.offset_rotation = 0;
        panel_cfg.invert        = true;
        _panel_instance.config(panel_cfg);

        // Backlight: GPIO 9
        auto light_cfg      = _light_instance.config();
        light_cfg.pin_bl    = 9;
        light_cfg.freq      = 44100;
        light_cfg.invert    = false;
        _light_instance.config(light_cfg);
        _panel_instance.setLight(&_light_instance);

        // Touch: FT5x06 on I2C, SDA=11, SCL=12, INT=14
        auto touch_cfg      = _touch_instance.config();
        touch_cfg.i2c_port  = 1;
        touch_cfg.i2c_addr  = 0x38;
        touch_cfg.pin_sda   = 11;
        touch_cfg.pin_scl   = 12;
        touch_cfg.pin_int   = 14;
        touch_cfg.freq      = 400000;
        touch_cfg.x_min     = 0;
        touch_cfg.x_max     = 239;
        touch_cfg.y_min     = 0;
        touch_cfg.y_max     = 239;
        _touch_instance.config(touch_cfg);
        _panel_instance.setTouch(&_touch_instance);

        setPanel(&_panel_instance);
    }
};

static LGFX_M5Dial display;

// ---------------------------------------------------------------------------
// Rotary Encoder (GPIO 40, 41) via ESP-IDF PCNT
// ---------------------------------------------------------------------------
static pcnt_unit_handle_t pcnt_unit = NULL;
static int enc_last_count = 0;

static void encoder_init() {
    pcnt_unit_config_t unit_config = {
        .low_limit  = -32768,
        .high_limit =  32767,
    };
    pcnt_new_unit(&unit_config, &pcnt_unit);

    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num  = 41,
        .level_gpio_num = 40,
    };
    pcnt_channel_handle_t pcnt_chan_a = NULL;
    pcnt_new_channel(pcnt_unit, &chan_a_config, &pcnt_chan_a);
    pcnt_channel_set_edge_action(pcnt_chan_a,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(pcnt_chan_a,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    pcnt_chan_config_t chan_b_config = {
        .edge_gpio_num  = 40,
        .level_gpio_num = 41,
    };
    pcnt_channel_handle_t pcnt_chan_b = NULL;
    pcnt_new_channel(pcnt_unit, &chan_b_config, &pcnt_chan_b);
    pcnt_channel_set_edge_action(pcnt_chan_b,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(pcnt_chan_b,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    pcnt_unit_enable(pcnt_unit);
    pcnt_unit_clear_count(pcnt_unit);
    pcnt_unit_start(pcnt_unit);
}

static int32_t encoder_read_and_reset() {
    int count;
    pcnt_unit_get_count(pcnt_unit, &count);
    int32_t delta = count - enc_last_count;
    enc_last_count = count;
    return delta;
}

// ---------------------------------------------------------------------------
// Speaker / Buzzer (GPIO 3) via LEDC
// ---------------------------------------------------------------------------
#define SPEAKER_GPIO          3
#define SPEAKER_LEDC_CHANNEL  LEDC_CHANNEL_0
#define SPEAKER_LEDC_TIMER    LEDC_TIMER_0
static uint8_t speaker_volume = 180; // 0-255

static uint32_t speaker_stop_at = 0;

static void speaker_init() {
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = SPEAKER_LEDC_TIMER,
        .freq_hz         = 1000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t chan_conf = {
        .gpio_num   = SPEAKER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = SPEAKER_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = SPEAKER_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&chan_conf);
}

// Non-blocking tone: sets frequency and duty, schedules stop
static void speaker_tone_nb(uint16_t freq, uint16_t duration_ms) {
    if (freq == 0) return;
    ledc_set_freq(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_TIMER, freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL, speaker_volume / 2);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL);
    speaker_stop_at = millis() + duration_ms;
}

// Call in main loop to stop tone when duration expires
static void speaker_update() {
    if (speaker_stop_at && millis() >= speaker_stop_at) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL);
        speaker_stop_at = 0;
    }
}

// Blocking tone (used in starburst effect)
static void speaker_tone_blocking(uint16_t freq, uint16_t duration_ms) {
    if (freq == 0) return;
    ledc_set_freq(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_TIMER, freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL, speaker_volume / 2);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL);
}

// ---------------------------------------------------------------------------
// Button A (GPIO 42, active low with pullup)
// ---------------------------------------------------------------------------
#define BTN_A_GPIO 42
static bool btn_prev = true;
static bool btn_was_pressed = false;
static uint32_t btn_press_start = 0;
static bool btn_was_hold = false;

static void button_init() {
    gpio_config_t io_conf = {
        .pin_bit_mask   = (1ULL << BTN_A_GPIO),
        .mode           = GPIO_MODE_INPUT,
        .pull_up_en     = GPIO_PULLUP_ENABLE,
        .pull_down_en   = GPIO_PULLDOWN_DISABLE,
        .intr_type      = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

static void button_update() {
    bool cur = gpio_get_level((gpio_num_t)BTN_A_GPIO);
    btn_was_pressed = false;
    btn_was_hold = false;
    if (!cur && btn_prev) { // falling edge = press
        btn_press_start = millis();
    }
    if (cur && !btn_prev) { // rising edge = release
        uint32_t dur = millis() - btn_press_start;
        if (dur > 800) {
            btn_was_hold = true;
        } else if (dur > 30) {
            btn_was_pressed = true;
        }
    }
    btn_prev = cur;
}

// ---------------------------------------------------------------------------
// Color utilities
// ---------------------------------------------------------------------------
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static inline uint16_t dim_color(uint16_t c, float k)
{
    uint8_t r5 = (c >> 11) & 0x1F; uint8_t g6 = (c >> 5) & 0x3F; uint8_t b5 = c & 0x1F;
    uint8_t r8 = r5 << 3; uint8_t g8 = g6 << 2; uint8_t b8 = b5 << 3;
    r8 = (uint8_t)(r8 * k); g8 = (uint8_t)(g8 * k); b8 = (uint8_t)(b8 * k);
    return rgb(r8, g8, b8);
}

// ---------------------------------------------------------------------------
// Configuration constants
// ---------------------------------------------------------------------------
namespace Config
{
    // Rotary encoder / brightness
    static constexpr int BrightMax = 100;
    static constexpr int BrightStep = 10;
    static constexpr int EncDiv = 4;

    // Audio: click on rotation
    static constexpr uint16_t ClickUpFreq = 1800;
    static constexpr uint16_t ClickDownFreq = 1000;
    static constexpr uint16_t ClickMs = 40;

    // Audio: confirm tones for theme/mute actions
    static constexpr uint16_t ConfirmToneUpFreq = 1800;
    static constexpr uint16_t ConfirmToneDownFreq = 1200;
    static constexpr uint16_t ConfirmTone1Ms = 70;
    static constexpr uint16_t ConfirmTone2Ms = 90;
    static constexpr uint16_t ConfirmToneGapMs = 80;

    // Audio: invert (long-press) two-tone "be-boop"
    static constexpr uint16_t InvertTone1Freq = 900;
    static constexpr uint16_t InvertTone2Freq = 600;
    static constexpr uint16_t InvertTone1Ms = 80;
    static constexpr uint16_t InvertTone2Ms = 100;
    static constexpr uint16_t InvertToneGapMs = 70;

    // Starburst effect configuration
    static constexpr int StarburstRays = 16;
    static constexpr int StarburstSteps = 12;
    static constexpr int StarburstDelayMs = 14;
    static constexpr uint16_t StarburstTone1Freq = 1500;
    static constexpr uint16_t StarburstTone2Freq = 2100;
    static constexpr uint16_t StarburstTone1Ms = 60;
    static constexpr uint16_t StarburstTone2Ms = 70;
    static constexpr uint16_t StarburstToneGapMs = 50;

    // Audio: tap pop effect
    static constexpr uint16_t TapPop1Freq = 1200;
    static constexpr uint16_t TapPop2Freq = 1800;
    static constexpr uint16_t TapPop1Ms = 50;
    static constexpr uint16_t TapPop2Ms = 60;
    static constexpr uint16_t TapPopGapMs = 60;
    static constexpr uint8_t SpeakerVolume = 180;

    // Touch / gestures
    static constexpr uint16_t TouchHoldThreshMs = 1000;
    static constexpr uint16_t TouchFlickThresh = 18;
    static constexpr uint16_t LongPressInvertMs = 1000;
    static constexpr uint16_t TapMaxReleaseMs = 250;
    static constexpr uint16_t TapMaxMovePx = 20;

    // Crosshair overlay
    static constexpr int CrosshairRadius = 12;

    // Target ping animation
    static constexpr int PingStep = 12;
    static constexpr int PingIntervalMs = 16;

    // Ring scale
    static constexpr int RingTicks = 100;
    static constexpr float RingStartDeg = -90.0f;
    static constexpr float RingSweepDeg = 360.0f;
    static constexpr int TickLenMajor = 18;
    static constexpr int TickLenMinor = 12;
    static constexpr int TickMajorEvery = 10;
    static constexpr int EdgeMargin = 6;

#ifdef RELEASE_BUILD
    static constexpr bool DebugTouch = false;
    static constexpr bool DebugHeartbeat = false;
    static constexpr bool DebugBtn = false;
    static constexpr bool DebugRot = false;
    static constexpr bool DebugPing = false;
#else
    static constexpr bool DebugTouch = true;
    static constexpr bool DebugHeartbeat = false;
    static constexpr bool DebugBtn = true;
    static constexpr bool DebugRot = true;
    static constexpr bool DebugPing = true;
#endif
}

// ---------------------------------------------------------------------------
// Themes
// ---------------------------------------------------------------------------
struct Theme
{
    const char *name;
    uint16_t bg;
    uint16_t primary;
    uint16_t accent;
    uint16_t text;
    uint16_t ripple;
};

static Theme THEMES[] = {
    {"Carbon", rgb(8, 8, 8),    rgb(0, 180, 255), rgb(255, 255, 255), rgb(255, 160, 60),    rgb(255, 220, 0)},
    {"Neon",   rgb(10, 6, 18),  rgb(0, 255, 170), rgb(255, 0, 180),  rgb(255, 120, 200),   rgb(255, 255, 0)},
    {"Ember",  rgb(24, 10, 32), rgb(255, 110, 0), rgb(255, 40, 80),  rgb(80, 160, 255),    rgb(255, 200, 0)},
    {"Ocean",  rgb(4, 10, 26),  rgb(0, 150, 255), rgb(220, 240, 255),rgb(255, 160, 60),    rgb(0, 220, 255)},
    {"Lime",   rgb(12, 18, 12), rgb(140, 255, 80),rgb(255, 255, 255),rgb(255, 120, 180),   rgb(170, 255, 120)},
    {"Sunset", rgb(20, 6, 28),  rgb(255, 200, 0), rgb(255, 60, 60),  rgb(180, 120, 255),   rgb(255, 220, 120)},
};

static const int NUM_THEMES = (int)(sizeof(THEMES) / sizeof(THEMES[0]));

// ---------------------------------------------------------------------------
// State variables
// ---------------------------------------------------------------------------
static int theme_idx = 0;
static bool mute = false;
static bool invert_latched = false;

static int brightness_pct = 80;
static int last_ring_brightness = -1;
static int last_ring_theme = -1;

static int enc_accum = 0;
static int32_t enc_total = 0;

static uint32_t tone2_at = 0;
static uint16_t tone2_freq = 0;
static uint16_t tone2_dur = 0;

static uint32_t press_start_ms = 0;
static int16_t press_x0 = 0, press_y0 = 0;
static bool ripple_active = false;
static int16_t ripple_x = 0, ripple_y = 0, ripple_r = 10, ripple_prev_r = 0;
static uint32_t ripple_redraw_at = 0;
static bool touch_active = false;
static bool touch_dragged = false;
static int16_t touch_last_x = 0, touch_last_y = 0;

// Ping overlay background snapshot
static uint16_t *ping_bk = nullptr;
static int ping_w = 0, ping_h = 0;
static int ping_x0 = 0, ping_y0 = 0;

// Crosshair pointer state
static int16_t cross_cx = 0, cross_cy = 0, cross_r = Config::CrosshairRadius;
static int16_t cross_prev_cx = 0, cross_prev_cy = 0;
static bool cross_initialized = false;
// Crosshair overlay background snapshot
static uint16_t *cross_bk = nullptr;
static int cross_w = 0, cross_h = 0;
static int cross_prev_x = 0, cross_prev_y = 0;

static int cx = 120, cy = 120;
static int ring_ro = 102, ring_ri = 80;
static int play_area_r = 64;

// ----------------------------------------------------------------------------
// Phase-4 ruler state (M5Dial = ruler in the hive)
// ----------------------------------------------------------------------------
typedef enum { BLE_UI_OFF, BLE_UI_ADVERT, BLE_UI_CONNECTED } ble_ui_t;
typedef enum { WIFI_UI_OFF, WIFI_UI_CONNECTING, WIFI_UI_CONNECTED, WIFI_UI_FAILED } wifi_ui_t;

static volatile ble_ui_t   ble_ui  = BLE_UI_OFF;
static volatile wifi_ui_t  wifi_ui = WIFI_UI_OFF;
static char                hive_ip_str[20]     = "N/A";
static volatile bool       ble_teardown_requested = false;
static volatile bool       ble_torn_down          = false;
static bool                sntp_started           = false;
static bool                time_synced            = false;
#define TIME_SYNC_EPOCH_THRESHOLD 1577836800 // 2020-01-01

// Ruler peer table. Populated by the on_hello callback; polled from the
// main loop for the status bar. 8 slots matches craw_hive_ruler's MAX_SESSIONS.
#define MAX_PEERS 8
struct PeerRec {
    bool     in_use;
    char     node_id[33];
    char     role[17];
    int64_t  joined_ms;
};
static PeerRec peers[MAX_PEERS] = {};
static int peer_count = 0;
static bool ruler_started = false;

static void peers_record(const char *node_id, const char *role) {
    int64_t now_ms = esp_timer_get_time() / 1000;
    // Update existing or add new
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].in_use && strcmp(peers[i].node_id, node_id) == 0) {
            strncpy(peers[i].role, role, sizeof(peers[i].role) - 1);
            peers[i].joined_ms = now_ms;
            return;
        }
    }
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!peers[i].in_use) {
            peers[i].in_use = true;
            strncpy(peers[i].node_id, node_id, sizeof(peers[i].node_id) - 1);
            strncpy(peers[i].role,    role,    sizeof(peers[i].role) - 1);
            peers[i].joined_ms = now_ms;
            peer_count++;
            return;
        }
    }
}

// Sleep / wake state. When true, display backlight is off and main loop
// skips rendering. Any encoder tick, touch press, or button press returns
// the display to the normal dial program via wake_from_sleep().
static bool app_sleeping = false;
// When showing a full-screen info overlay (appshowmem / appdevinfo), the
// main loop suspends rendering until the next touch/encoder/button event
// restores the dial scene.
static bool info_overlay_active = false;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void draw_status();
static void draw_ring(bool force = false);
static void draw_center_label();
static void draw_scene(bool force = true);
static void draw_crosshair_overlay(int16_t x, int16_t y, bool recapture_only = false);
static void play_pop();
static void play_confirm_up();
static void play_confirm_down();
static void play_invert();
static void effect_starburst();

// ---------------------------------------------------------------------------
// Phase-4 ruler: WiFi / BLE provisioning / hive callbacks
// ---------------------------------------------------------------------------
static char ruler_id[40]  = "MagNET-ruler";
static char ruler_mac4[8] = "0000";
static const char *HIVE_ID = "beehive-1";

static void derive_ruler_id(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ruler_mac4, sizeof(ruler_mac4), "%02x%02x", mac[4], mac[5]);
    snprintf(ruler_id,   sizeof(ruler_id),   "MagNET-ruler-%s", ruler_mac4);
}

static void on_hive_hello(const char *node_id, const char *role_requested,
                          const char *hive_id, bool *accept, char *role_out,
                          size_t role_out_len, void *ctx) {
    (void)ctx; (void)hive_id;
    // v1 consensus stub: accept everyone, grant the role they asked for.
    *accept = true;
    strncpy(role_out, role_requested, role_out_len - 1);
    role_out[role_out_len - 1] = '\0';
    peers_record(node_id, role_out);
    ESP_LOGI(TAG, "[HIVE] peer joined: %s as %s", node_id, role_out);
    usb_printf("[HIVE] peer joined: %s as %s\r\n", node_id, role_out);
}

static void maybe_start_ruler(void) {
    if (ruler_started) return;
    if (!craw_wifi_is_connected()) return;
    if (time(NULL) < TIME_SYNC_EPOCH_THRESHOLD) return;
    derive_ruler_id();
    static uint8_t secret[32];
    memcpy(secret, CRAW_HIVE_DEV_SECRET, 32);
    static craw_hive_ruler_config_t rcfg;
    rcfg = {};
    rcfg.port         = CRAW_HIVE_DEFAULT_PORT;
    rcfg.hive_id      = HIVE_ID;
    rcfg.ruler_id     = ruler_id;
    rcfg.secret       = secret;
    rcfg.on_hello     = on_hive_hello;
    rcfg.on_hello_ctx = nullptr;
    if (craw_hive_ruler_start(&rcfg) == 0) {
        ruler_started = true;
        usb_printf("[HIVE] ruler started as '%s' (hive=%s)\r\n", ruler_id, HIVE_ID);
        /* Pre-seed the local KV table with embedded bundles so peers can
         * KV_GET them without a Scribe present. Idempotent — safe to call
         * if some entries are already there. */
        bundle_bootstrap();
    } else {
        usb_print("[HIVE] ruler_start failed\r\n");
    }
}

static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    (void)ctx;
    switch (event) {
    case CRAW_WIFI_EVENT_CONNECTED:
        craw_wifi_get_ip_str(hive_ip_str, sizeof(hive_ip_str));
        wifi_ui = WIFI_UI_CONNECTED;
        usb_printf("\r\n[WiFi] connected, IP: %s\r\n", hive_ip_str);
        craw_ble_provision_set_ip(hive_ip_str);
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTED);
        // BLE still uses ~50 KB and couples into audio on some boards. The
        // Dial uses LEDC buzzer (not I2S amp) so coupling is less audible,
        // but the memory is still better spent on mDNS + TCP + hive.
        craw_ble_provision_stop_advertising();
        ble_ui = BLE_UI_OFF;
        ble_teardown_requested = true;
        if (!sntp_started) {
            esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sntp_cfg);
            sntp_started = true;
            usb_print("[SNTP] sync kicked off\r\n");
        }
        break;
    case CRAW_WIFI_EVENT_DISCONNECTED:
        wifi_ui = WIFI_UI_OFF;
        usb_print("\r\n[WiFi] disconnected\r\n");
        strncpy(hive_ip_str, "N/A", sizeof(hive_ip_str));
        if (!ble_torn_down) {
            craw_ble_provision_set_ip(hive_ip_str);
            craw_ble_provision_advertise();
            ble_ui = BLE_UI_ADVERT;
        }
        break;
    case CRAW_WIFI_EVENT_CONNECT_FAILED:
        wifi_ui = WIFI_UI_FAILED;
        usb_print("\r\n[WiFi] failed\r\n");
        strncpy(hive_ip_str, "N/A", sizeof(hive_ip_str));
        if (!ble_torn_down) {
            craw_ble_provision_set_ip(hive_ip_str);
            craw_ble_provision_set_status(CRAW_BLE_PROV_FAILED);
            craw_ble_provision_advertise();
            ble_ui = BLE_UI_ADVERT;
        }
        break;
    }
}

static void on_prov_event(craw_ble_prov_state_t state,
                          const char *ssid, const char *pass, void *ctx) {
    (void)ctx;
    switch (state) {
    case CRAW_BLE_PROV_CREDS_RECEIVED:
        ble_ui = BLE_UI_CONNECTED;
        usb_printf("\r\n[PROV] creds: ssid='%s'\r\n", ssid);
        break;
    case CRAW_BLE_PROV_COMMIT_REQUESTED:
        if (!ssid || !ssid[0]) break;
        usb_printf("\r\n[PROV] commit -> '%s'\r\n", ssid);
        craw_nvs_save_wifi_creds(ssid, pass ? pass : "");
        wifi_ui = WIFI_UI_CONNECTING;
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
        craw_wifi_connect(ssid, pass ? pass : "");
        break;
    default:
        break;
    }
}

// Background task: performs deferred BLE teardown (outside NimBLE callback
// context), polls SNTP completion, starts the ruler when all preconditions
// are satisfied. Runs at low priority so it never interferes with the
// playground loop / encoder / touch.
static void hive_housekeeping_task(void *arg) {
    (void)arg;
    while (1) {
        if (ble_teardown_requested && !ble_torn_down) {
            vTaskDelay(pdMS_TO_TICKS(500));
            size_t before = esp_get_free_heap_size();
            craw_ble_provision_deinit();
            ble_torn_down = true;
            size_t after = esp_get_free_heap_size();
            usb_printf("[BLE] torn down. Heap: %u -> %u (+%d bytes)\r\n",
                       (unsigned)before, (unsigned)after,
                       (int)after - (int)before);
        }
        if (sntp_started && !time_synced) {
            time_t now = time(NULL);
            if (now > TIME_SYNC_EPOCH_THRESHOLD) {
                time_synced = true;
                struct tm t;
                localtime_r(&now, &t);
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
                usb_printf("[SNTP] time synced: %s UTC\r\n", buf);
            }
        }
        maybe_start_ruler();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// ---------------------------------------------------------------------------
// Drawing functions
// ---------------------------------------------------------------------------
static void draw_status()
{
    (void)THEMES;
    (void)brightness_pct;
    (void)invert_latched;
    (void)mute;
}

// Tiny status bar near the top of the dial: 3 colored dots + peer count.
// Called from draw_center_label so every redraw reflects current state.
static void draw_hive_status_bar()
{
    auto &t = THEMES[theme_idx];
    int y = cy - 64;
    int dot_r = 4;
    // BLE dot
    uint16_t ble_col = t.bg;
    switch (ble_ui) {
        case BLE_UI_ADVERT:    ble_col = rgb(0, 200, 255); break; // cyan
        case BLE_UI_CONNECTED: ble_col = rgb(255, 255, 255); break; // white
        default: ble_col = rgb(40, 40, 40); break;
    }
    display.fillCircle(cx - 30, y, dot_r, ble_col);
    // WiFi dot
    uint16_t wifi_col = t.bg;
    switch (wifi_ui) {
        case WIFI_UI_CONNECTING: wifi_col = rgb(255, 180, 0); break;
        case WIFI_UI_CONNECTED:  wifi_col = rgb(0, 255, 80);  break;
        case WIFI_UI_FAILED:     wifi_col = rgb(255, 60, 60); break;
        default: wifi_col = rgb(40, 40, 40); break;
    }
    display.fillCircle(cx - 10, y, dot_r, wifi_col);
    // Hive dot (lit when ruler is running)
    uint16_t hive_col = ruler_started ? rgb(120, 255, 120) : rgb(40, 40, 40);
    display.fillCircle(cx + 10, y, dot_r, hive_col);
    // Peer count
    display.setTextDatum(lgfx::textdatum_t::middle_left);
    display.setTextColor(t.text, t.bg);
    display.setTextSize(1);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", peer_count);
    display.drawString(buf, cx + 20, y);
}

static void draw_center_label()
{
    auto &t = THEMES[theme_idx];
    draw_hive_status_bar();
    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextColor(t.primary, t.bg);
    display.setTextSize(2);
    const int total = NUM_THEMES;
    char line[64];
    snprintf(line, sizeof(line), "%s  (%d/%d)", t.name, theme_idx + 1, total);
    display.drawString(line, cx, cy - 14);

    display.setTextSize(1);
    display.setTextColor(t.text, t.bg);
    char instr1[48];
    snprintf(instr1, sizeof(instr1), "Rotate: brightness (%d%%)", Config::BrightStep);
    display.drawString(instr1, cx, cy + 0);
    display.drawString("Tap: ping  BtnA: theme  Hold: burst", cx, cy + 12);
    display.drawString("Long press: invert", cx, cy + 24);
    char pos[32];
    snprintf(pos, sizeof(pos), "X:%d  Y:%d", cross_cx, cross_cy);
    display.drawString(pos, cx, cy + 36);
}

static void draw_ring(bool force)
{
    if (!force && last_ring_brightness == brightness_pct && last_ring_theme == theme_idx)
        return;
    auto &t = THEMES[theme_idx];
    int outer_r = std::min(cx, cy) - 1;
    int clear_r = outer_r + 2;
    display.fillCircle(cx, cy, clear_r, t.bg);

    const int ticks = Config::RingTicks;
    const float start_deg = Config::RingStartDeg;
    const float sweep_deg = Config::RingSweepDeg;
    const int tick_len_major = Config::TickLenMajor;
    const int tick_len_minor = Config::TickLenMinor;
    const int major_every = Config::TickMajorEvery;
    uint16_t dim_unlit = dim_color(t.text, 0.35f);

    for (int i = 0; i < ticks; ++i)
    {
        float a = (start_deg + sweep_deg * (i / (float)ticks)) * (float)M_PI / 180.0f;
        int len = (i % major_every == 0) ? tick_len_major : tick_len_minor;
        int x1 = cx + (int)(cosf(a) * outer_r);
        int y1 = cy + (int)(sinf(a) * outer_r);
        int x0 = cx + (int)(cosf(a) * (outer_r - len));
        int y0 = cy + (int)(sinf(a) * (outer_r - len));
        display.drawLine(x0, y0, x1, y1, dim_unlit);
    }

    int lit = (int)(ticks * (brightness_pct / 100.0f) + 0.5f);
    if (lit > ticks) lit = ticks;
    if (lit < 0) lit = 0;
    for (int i = 0; i < lit; ++i)
    {
        float a = (start_deg + sweep_deg * (i / (float)ticks)) * (float)M_PI / 180.0f;
        int len = (i % major_every == 0) ? tick_len_major : tick_len_minor;
        int x1 = cx + (int)(cosf(a) * outer_r);
        int y1 = cy + (int)(sinf(a) * outer_r);
        int x0 = cx + (int)(cosf(a) * (outer_r - len));
        int y0 = cy + (int)(sinf(a) * (outer_r - len));
        display.drawLine(x0, y0, x1, y1, t.primary);
    }

    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextColor(t.primary, t.bg);
    display.setTextSize(3);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", brightness_pct);
    display.drawString(buf, cx, cy - 40);

    last_ring_brightness = brightness_pct;
    last_ring_theme = theme_idx;

    draw_center_label();
    if (cross_initialized)
        draw_crosshair_overlay(cross_cx, cross_cy, true);
}

static void draw_scene(bool force)
{
    auto &t = THEMES[theme_idx];
    display.fillScreen(t.bg);
    draw_ring(true);
    draw_center_label();
    if (cross_initialized)
        draw_crosshair_overlay(cross_cx, cross_cy, true);
}

static void draw_crosshair_overlay(int16_t x, int16_t y, bool recapture_only)
{
    auto &t = THEMES[theme_idx];
    int r = cross_r;
    int w = r * 2 + 1;
    int h = r * 2 + 1;
    int x0 = x - r;
    int y0 = y - r;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 + w > display.width())  w = display.width() - x0;
    if (y0 + h > display.height()) h = display.height() - y0;

    // Restore previous background if present and we're moving
    if (!recapture_only && cross_bk && cross_w > 0 && cross_h > 0)
    {
        display.pushImage(cross_prev_x, cross_prev_y, cross_w, cross_h, cross_bk);
    }

    // (Re)allocate buffer if size changed
    size_t need = (size_t)w * (size_t)h;
    if (need > (size_t)cross_w * (size_t)cross_h)
    {
        free(cross_bk);
        cross_bk = (uint16_t *)malloc(need * sizeof(uint16_t));
    }
    cross_w = w;
    cross_h = h;
    cross_prev_x = x0;
    cross_prev_y = y0;
    if (cross_bk)
    {
        display.readRect(x0, y0, w, h, cross_bk);
    }

    // Draw crosshair lines
    uint16_t c = t.accent;
    display.drawLine(x - r, y, x + r, y, c);
    display.drawLine(x, y - r, x, y + r, c);

    cross_cx = x;
    cross_cy = y;
}

// ---------------------------------------------------------------------------
// Sound helpers
// ---------------------------------------------------------------------------
static void play_pop()
{
    if (mute) return;
    speaker_tone_nb(Config::TapPop1Freq, Config::TapPop1Ms);
    tone2_at = millis() + Config::TapPopGapMs;
    tone2_freq = Config::TapPop2Freq;
    tone2_dur = Config::TapPop2Ms;
}

static void play_confirm_up()
{
    if (mute) return;
    speaker_tone_nb(Config::ConfirmToneDownFreq, Config::ConfirmTone1Ms);
    tone2_at = millis() + Config::ConfirmToneGapMs;
    tone2_freq = Config::ConfirmToneUpFreq;
    tone2_dur = Config::ConfirmTone2Ms;
}

static void play_confirm_down()
{
    if (mute) return;
    speaker_tone_nb(Config::ConfirmToneUpFreq, Config::ConfirmTone1Ms);
    tone2_at = millis() + Config::ConfirmToneGapMs;
    tone2_freq = Config::ConfirmToneDownFreq;
    tone2_dur = Config::ConfirmTone2Ms;
}

static void play_invert()
{
    if (mute) return;
    speaker_tone_nb(Config::InvertTone1Freq, Config::InvertTone1Ms);
    tone2_at = millis() + Config::InvertToneGapMs;
    tone2_freq = Config::InvertTone2Freq;
    tone2_dur = Config::InvertTone2Ms;
}

// ---------------------------------------------------------------------------
// Starburst effect (blocking animation — short duration)
// ---------------------------------------------------------------------------
static void effect_starburst()
{
    auto &t = THEMES[theme_idx];
    int outer_r = std::min(cx, cy) - 1;
    const int rays = Config::StarburstRays;
    const int steps = Config::StarburstSteps;
    const int delay_ms = Config::StarburstDelayMs;

    // Optional: small upbeat chirp
    if (!mute) {
        speaker_tone_nb(Config::StarburstTone1Freq, Config::StarburstTone1Ms);
        tone2_at = millis() + Config::StarburstToneGapMs;
        tone2_freq = Config::StarburstTone2Freq;
        tone2_dur = Config::StarburstTone2Ms;
    }
    ESP_LOGD(TAG, "[EFFECT] Starburst start");

    int prev_len = 0;
    for (int s = 1; s <= steps; ++s)
    {
        int len = (outer_r * s) / steps;
        for (int i = 0; i < rays; ++i)
        {
            float a = (2.0f * (float)M_PI * i) / rays;
            int x1 = cx + (int)(cosf(a) * len);
            int y1 = cy + (int)(sinf(a) * len);
            uint16_t col = (i % 2 == 0) ? t.primary : t.accent;
            display.drawLine(cx, cy, x1, y1, col);
        }
        if (prev_len > 0)
        {
            for (int i = 0; i < rays; ++i)
            {
                float a = (2.0f * (float)M_PI * i) / rays;
                int x1 = cx + (int)(cosf(a) * prev_len);
                int y1 = cy + (int)(sinf(a) * prev_len);
                display.drawLine(cx, cy, x1, y1, t.bg);
            }
        }
        prev_len = len;
        button_update();
        speaker_update();
        taskYIELD();
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    // Retract quickly
    for (int s = steps; s >= 0; --s)
    {
        for (int i = 0; i < rays; ++i)
        {
            float a = (2.0f * (float)M_PI * i) / rays;
            int x1 = cx + (int)(cosf(a) * prev_len);
            int y1 = cy + (int)(sinf(a) * prev_len);
            display.drawLine(cx, cy, x1, y1, t.bg);
        }
        int len = (outer_r * s) / steps;
        for (int i = 0; i < rays; ++i)
        {
            float a = (2.0f * (float)M_PI * i) / rays;
            int x1 = cx + (int)(cosf(a) * len);
            int y1 = cy + (int)(sinf(a) * len);
            uint16_t col = (i % 2 == 0) ? t.primary : t.accent;
            display.drawLine(cx, cy, x1, y1, col);
        }
        prev_len = len;
        button_update();
        speaker_update();
        taskYIELD();
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    draw_ring(true);
    ESP_LOGD(TAG, "[EFFECT] Starburst end");
}

// ---------------------------------------------------------------------------
// Forth REPL task
// ---------------------------------------------------------------------------
static void forth_repl_task(void *arg)
{
    (void)arg;
    forth_repl(uart_getchar, uart_putchar);
    // Should not return, but if it does:
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Forth FFI Words
// ---------------------------------------------------------------------------

// ( n -- ) Set theme index (0-based, wraps)
static void w_theme(void) {
    int n = (int)forth_pop();
    theme_idx = n % NUM_THEMES;
    if (theme_idx < 0) theme_idx += NUM_THEMES;
    draw_scene(true);
}

// ( n -- ) Set brightness percentage (0-100)
static void w_brightness(void) {
    int n = (int)forth_pop();
    if (n < 0) n = 0;
    if (n > Config::BrightMax) n = Config::BrightMax;
    brightness_pct = n;
    int mapped = (brightness_pct * 255) / Config::BrightMax;
    display.setBrightness(mapped);
    draw_ring(false);
}

// ( -- ) Trigger ping animation at center
static void w_ping(void) {
    ripple_x = (int16_t)cx;
    ripple_y = (int16_t)cy;
    ripple_prev_r = 0;
    ripple_r = 6;
    ripple_active = true;
    ripple_redraw_at = millis() + Config::PingIntervalMs;
    if (!mute) play_pop();
}

// ( -- ) Trigger starburst effect
static void w_starburst(void) {
    effect_starburst();
}

// ( -- ) Toggle display inversion
static void w_invert(void) {
    invert_latched = !invert_latched;
    display.invertDisplay(invert_latched);
    if (!mute) play_invert();
}

// ( -- ) Toggle mute
static void w_mute(void) {
    mute = !mute;
    if (!mute) play_confirm_up();
}

// ( -- n ) Push current theme index
static void w_theme_get(void) {
    forth_push(theme_idx);
}

// ( -- n ) Push current brightness
static void w_bright_get(void) {
    forth_push(brightness_pct);
}

// ---------------------------------------------------------------------------
// Sleep / wake helpers (shared by appsleep, appshowmem, appdevinfo)
// ---------------------------------------------------------------------------
static void wake_from_sleep(void) {
    if (!app_sleeping && !info_overlay_active) return;
    app_sleeping = false;
    info_overlay_active = false;
    int mapped = (brightness_pct * 255) / Config::BrightMax;
    display.setBrightness(mapped);
    draw_scene(true);
}

// ( -- ) Play a short beep. Ignores mute so this word is deterministic
// and can serve as a Forth-level "it's alive" tone.
static void w_appbeep(void) {
    speaker_tone_nb(2000, 120);
}

// ( -- ) Blank the display and back-off brightness until the user
// interacts with the dial, touches the screen, or presses the button.
// Wake is handled in the main loop.
static void w_appsleep(void) {
    app_sleeping = true;
    info_overlay_active = false;
    ripple_active = false;
    free(ping_bk); ping_bk = nullptr; ping_w = ping_h = 0;
    auto &t = THEMES[theme_idx];
    display.fillScreen(t.bg);
    display.setBrightness(0);
}

// ( -- ) Dump memory state to the display and serial. Any touch, encoder
// tick, or button press restores the normal dial scene.
static void w_appshowmem(void) {
    auto &t = THEMES[theme_idx];
    info_overlay_active = true;
    app_sleeping = false;
    ripple_active = false;
    free(ping_bk); ping_bk = nullptr; ping_w = ping_h = 0;
    display.fillScreen(t.bg);
    display.setTextDatum(lgfx::textdatum_t::top_left);
    display.setTextColor(t.primary, t.bg);
    display.setTextSize(2);
    display.drawString("MEM", 10, 14);

    display.setTextSize(1);
    display.setTextColor(t.text, t.bg);

    size_t free_total    = esp_get_free_heap_size();
    size_t min_free      = esp_get_minimum_free_heap_size();
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    char buf[64];
    int y = 50;
    snprintf(buf, sizeof(buf), "free:     %u", (unsigned)free_total);
    display.drawString(buf, 10, y); y += 14;
    snprintf(buf, sizeof(buf), "internal: %u", (unsigned)free_internal);
    display.drawString(buf, 10, y); y += 14;
    snprintf(buf, sizeof(buf), "min free: %u", (unsigned)min_free);
    display.drawString(buf, 10, y); y += 14;
    snprintf(buf, sizeof(buf), "largest:  %u", (unsigned)largest_block);
    display.drawString(buf, 10, y); y += 14;
    snprintf(buf, sizeof(buf), "uptime:   %lus",
             (unsigned long)(esp_timer_get_time() / 1000000));
    display.drawString(buf, 10, y); y += 18;

    display.setTextColor(t.accent, t.bg);
    display.drawString("touch / turn / press to exit", 10, y);

    usb_printf("[MEM] free=%u internal=%u min=%u largest=%u\r\n",
               (unsigned)free_total, (unsigned)free_internal,
               (unsigned)min_free, (unsigned)largest_block);
}

// ( -- ) Dump chip/device info to the display and serial. Any touch, encoder
// tick, or button press restores the normal dial scene.
static void w_appdevinfo(void) {
    auto &t = THEMES[theme_idx];
    info_overlay_active = true;
    app_sleeping = false;
    ripple_active = false;
    free(ping_bk); ping_bk = nullptr; ping_w = ping_h = 0;
    display.fillScreen(t.bg);
    display.setTextDatum(lgfx::textdatum_t::top_left);
    display.setTextColor(t.primary, t.bg);
    display.setTextSize(2);
    display.drawString("DEV", 10, 14);

    display.setTextSize(1);
    display.setTextColor(t.text, t.bg);

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    const char *model = "unknown";
    switch (ci.model) {
        case CHIP_ESP32:   model = "ESP32";    break;
        case CHIP_ESP32S2: model = "ESP32-S2"; break;
        case CHIP_ESP32S3: model = "ESP32-S3"; break;
        case CHIP_ESP32C3: model = "ESP32-C3"; break;
        case CHIP_ESP32C6: model = "ESP32-C6"; break;
        case CHIP_ESP32H2: model = "ESP32-H2"; break;
        default: break;
    }

    char buf[64];
    int y = 50;
    snprintf(buf, sizeof(buf), "chip:   %s rev%d", model, ci.revision);
    display.drawString(buf, 10, y); y += 14;
    snprintf(buf, sizeof(buf), "cores:  %d", ci.cores);
    display.drawString(buf, 10, y); y += 14;
    snprintf(buf, sizeof(buf), "flash:  %luKB", (unsigned long)(flash_size / 1024));
    display.drawString(buf, 10, y); y += 14;
    snprintf(buf, sizeof(buf), "mac:    %02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    display.drawString(buf, 10, y); y += 14;
    snprintf(buf, sizeof(buf), "idf:    %s", esp_get_idf_version());
    display.drawString(buf, 10, y); y += 14;
    snprintf(buf, sizeof(buf), "forth:  %s", ESPIDFORTH_VERSION_STRING);
    display.drawString(buf, 10, y); y += 18;

    display.setTextColor(t.accent, t.bg);
    display.drawString("touch / turn / press to exit", 10, y);

    usb_printf("[DEV] %s rev%d cores=%d flash=%luKB mac=%02X%02X%02X%02X%02X%02X\r\n",
               model, ci.revision, ci.cores, (unsigned long)(flash_size / 1024),
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ( -- ) Print BLE / WiFi / ruler state to USB serial.
static void w_prov_status(void) {
    char ssid[33], pass[65];
    bool has = craw_nvs_load_wifi_creds(ssid, pass);
    usb_printf("\r\nble:    %s\r\n", craw_ble_provision_device_name());
    usb_printf("wifi:   %s\r\n", craw_wifi_is_connected() ? "connected" : "down");
    usb_printf("ssid:   %s\r\n", has ? ssid : "(none)");
    usb_printf("ip:     %s\r\n", hive_ip_str);
    usb_printf("time:   %s\r\n", time_synced ? "synced" : "pending");
    usb_printf("ruler:  %s\r\n", ruler_started ? ruler_id : "(not started)");
}

// ( -- ) Clear stored WiFi creds and reboot so BLE provisioning restarts.
static void w_prov_reset(void) {
    char ssid[33], pass[65];
    craw_nvs_clear_wifi_creds(ssid, pass);
    usb_print("\r\nCreds cleared. Rebooting to re-enter provisioning...\r\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

// KV REPL helpers — interactively read a key (and optional value) from
// USB-JTAG, then call into the ruler's local KV table. The local-table
// access lets the Dial seed/inspect data the rest of the hive can fetch
// via KV_GET messages.
static char w_io_key[CRAW_HIVE_KV_KEY_MAX + 1];
static char w_io_val[CRAW_HIVE_KV_VALUE_MAX + 1];

static void read_line(const char *prompt, char *buf, size_t bufsz) {
    usb_print(prompt);
    size_t i = 0;
    while (i + 1 < bufsz) {
        int c = uart_getchar();
        if (c < 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (c == '\r' || c == '\n') break;
        if (c == 8 || c == 127) {
            if (i > 0) { i--; usb_print("\b \b"); }
            continue;
        }
        buf[i++] = (char)c;
        char ch[2] = { (char)c, 0 };
        usb_print(ch);
    }
    buf[i] = '\0';
    usb_print("\r\n");
}

// ( -- ) Prompt for key + value, store in ruler's local KV table.
static void w_kv_set(void) {
    read_line("\r\nkey:   ", w_io_key, sizeof(w_io_key));
    read_line("value: ",     w_io_val, sizeof(w_io_val));
    int rc = craw_hive_ruler_kv_put(w_io_key, w_io_val);
    if (rc == 0) usb_printf("stored '%s'\r\n", w_io_key);
    else         usb_printf("kv-set failed rc=%d\r\n", rc);
}

// ( -- ) Prompt for key, print the stored value (local table only).
static void w_kv_get(void) {
    read_line("\r\nkey: ", w_io_key, sizeof(w_io_key));
    int rc = craw_hive_ruler_kv_get(w_io_key, w_io_val, sizeof(w_io_val));
    if (rc == 0) usb_printf("'%s' = '%s'\r\n", w_io_key, w_io_val);
    else         usb_print("not found\r\n");
}

static int kv_list_cb(const char *key, const char *value, void *ctx) {
    (void)ctx;
    /* Truncate value to keep the line readable. */
    char preview[64];
    strncpy(preview, value, sizeof(preview) - 1);
    preview[sizeof(preview) - 1] = '\0';
    usb_printf("  %-20s = %s\r\n", key, preview);
    return 0;
}

// ( -- ) List all entries in the ruler's local KV table.
static void w_kv_list(void) {
    usb_print("\r\nkv table:\r\n");
    int n = craw_hive_ruler_kv_iterate(kv_list_cb, NULL);
    usb_printf("(%d entries)\r\n", n);
}

// ( -- ) Send a ROLE_GRANT to a connected peer. Prompts for node_id, the
// new role label, and an optional bundle key (empty for label-only). The
// peer's on_role_grant callback dispatches the install pipeline.
static char w_grant_role_buf[CRAW_HIVE_KV_KEY_MAX + 1];
static void w_grant_role(void) {
    read_line("\r\nnode-id:    ", w_io_key, sizeof(w_io_key));
    read_line("role:       ",     w_io_val, sizeof(w_io_val));
    read_line("bundle key (blank for none): ", w_grant_role_buf, sizeof(w_grant_role_buf));
    const char *bundle = w_grant_role_buf[0] ? w_grant_role_buf : NULL;
    int rc = craw_hive_ruler_grant_role(w_io_key, w_io_val, bundle, "*");
    if      (rc == 0)  usb_printf("ROLE_GRANT sent to %s (role=%s bundle=%s)\r\n",
                                  w_io_key, w_io_val, bundle ? bundle : "(none)");
    else if (rc == -1) usb_printf("peer '%s' not connected\r\n", w_io_key);
    else               usb_printf("send failed rc=%d\r\n", rc);
}

// ( -- ) Print the peer table to USB serial.
static void w_ruler_status(void) {
    usb_printf("\r\nruler:   %s\r\n", ruler_started ? ruler_id : "(not started)");
    usb_printf("hive:    %s\r\n", HIVE_ID);
    usb_printf("peers:   %d\r\n", peer_count);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!peers[i].in_use) continue;
        int64_t ago_s = (esp_timer_get_time() / 1000 - peers[i].joined_ms) / 1000;
        usb_printf("  [%d] %-32s %-12s %llds ago\r\n",
                   i, peers[i].node_id, peers[i].role, (long long)ago_s);
    }
}

static void register_forth_words(void) {
    forth_register_word("theme",        w_theme);
    forth_register_word("brightness",   w_brightness);
    forth_register_word("ping",         w_ping);
    forth_register_word("starburst",    w_starburst);
    forth_register_word("invert",       w_invert);
    forth_register_word("mute",         w_mute);
    forth_register_word("theme?",       w_theme_get);
    forth_register_word("bright?",      w_bright_get);
    forth_register_word("appbeep",      w_appbeep);
    forth_register_word("appsleep",     w_appsleep);
    forth_register_word("appshowmem",   w_appshowmem);
    forth_register_word("appdevinfo",   w_appdevinfo);
    forth_register_word("prov-status",  w_prov_status);
    forth_register_word("prov-reset",   w_prov_reset);
    forth_register_word("ruler-status", w_ruler_status);
    forth_register_word("kv-set",       w_kv_set);
    forth_register_word("kv-get",       w_kv_get);
    forth_register_word("kv-list",      w_kv_list);
    forth_register_word("grant-role",   w_grant_role);
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
extern "C" void app_main(void)
{
    // USB serial setup
    setup_usb_serial();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Print boot banner
    usb_print("\r\n\r\n");
    usb_print("============================================\r\n");
    usb_printf("  M5Dial Demo ESPIDFORTH v%s\r\n", ESPIDFORTH_VERSION_STRING);
    usb_printf("  Build: %s %s\r\n", ESPIDFORTH_BUILD_DATE, ESPIDFORTH_BUILD_TIME);
    usb_print("  Playground demo with Forth REPL\r\n");
    usb_print("============================================\r\n");

    // Init hardware
    display.init();
    encoder_init();
    speaker_init();
    button_init();
    usb_print("Hardware initialized.\r\n");

    // Compute layout from display dimensions
    cx = display.width() / 2;
    cy = display.height() / 2;
    ring_ro = std::min(cx, cy) - Config::EdgeMargin;
    ring_ri = ring_ro - Config::TickLenMajor;
    play_area_r = ring_ri - 16;
    cross_cx = cx;
    cross_cy = cy;
    cross_prev_cx = cross_cx;
    cross_prev_cy = cross_cy;
    cross_initialized = true;

    // Initial scene
    display.fillScreen(THEMES[theme_idx].bg);
    draw_scene(true);

    // Set initial brightness
    int mapped = (brightness_pct * 255) / Config::BrightMax;
    display.setBrightness(mapped);

    // Set speaker volume and play boot tone
    speaker_volume = Config::SpeakerVolume;
    if (!mute)
        speaker_tone_nb(2000, 200);

    // Init Forth engine + register custom words
    forth_init(64 * 1024);
    register_forth_words();
    usb_print("Forth engine initialized.\r\n");
    usb_printf("Free heap: %lu bytes\r\n\r\n",
        (unsigned long)esp_get_free_heap_size());

    // ----- Phase-4 ruler: NVS + WiFi + BLE provisioning -----
    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    craw_wifi_init("MagNET-ruler", on_wifi_event, NULL);
    craw_ble_provision_config_t pcfg = {};
    pcfg.name_prefix = "MagNET-ruler";
    pcfg.role        = "ruler";
    craw_ble_provision_init(&pcfg, on_prov_event, NULL);
    ble_ui = BLE_UI_ADVERT;
    usb_printf("BLE: %s\r\n", craw_ble_provision_device_name());

    xTaskCreate(hive_housekeeping_task, "hive_hk", 6144, NULL, 4, NULL);

    // Auto-connect if creds stored from a previous boot
    {
        char ssid[33], pass[65];
        if (craw_nvs_load_wifi_creds(ssid, pass) && ssid[0]) {
            usb_printf("Stored WiFi '%s' — auto-connect\r\n", ssid);
            wifi_ui = WIFI_UI_CONNECTING;
            craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
            craw_wifi_connect(ssid, pass);
        } else {
            usb_print("No stored WiFi — provision via BLE before ruler can start.\r\n");
        }
    }

    usb_print("Forth commands:\r\n");
    usb_print("  N theme      -- set theme (0-5)\r\n");
    usb_print("  N brightness -- set brightness (0-100)\r\n");
    usb_print("  ping         -- trigger ping at center\r\n");
    usb_print("  starburst    -- trigger starburst effect\r\n");
    usb_print("  invert       -- toggle display inversion\r\n");
    usb_print("  mute         -- toggle mute\r\n");
    usb_print("  theme?       -- show current theme\r\n");
    usb_print("  bright?      -- show current brightness\r\n");
    usb_print("  appbeep      -- play a short beep\r\n");
    usb_print("  appsleep     -- blank display until touch/turn/press\r\n");
    usb_print("  appshowmem   -- display heap state\r\n");
    usb_print("  appdevinfo   -- display chip / CPU info\r\n");
    usb_print("  prov-status  -- BLE / WiFi / IP / ruler state\r\n");
    usb_print("  prov-reset   -- clear WiFi, reboot into BLE provisioning\r\n");
    usb_print("  ruler-status -- list connected hive peers\r\n\r\n");

    // Start Forth REPL in background task
    xTaskCreate(forth_repl_task, "forth_repl", 8192, NULL, 3, NULL);

    // Read and discard any initial encoder counts from boot
    encoder_read_and_reset();

    // Main loop
    while (1)
    {
        constexpr int BRIGHT_MAX = Config::BrightMax;
        constexpr int BRIGHT_STEP = Config::BrightStep;

        // Encoder -> brightness (also wakes from sleep / clears info overlay)
        int32_t d = encoder_read_and_reset();
        if (d) {
            if (app_sleeping || info_overlay_active) {
                wake_from_sleep();
                enc_accum = 0; // swallow the wake gesture, don't adjust brightness
                d = 0;
            } else {
                enc_accum += d;
            }
        }
        int logical = 0;
        while (enc_accum >= Config::EncDiv)
        {
            enc_accum -= Config::EncDiv;
            ++logical;
        }
        while (enc_accum <= -Config::EncDiv)
        {
            enc_accum += Config::EncDiv;
            --logical;
        }
        if (logical)
        {
            enc_total += logical;
            int prev_b = brightness_pct;
            brightness_pct += logical * BRIGHT_STEP;
            if (brightness_pct < 0) brightness_pct = 0;
            if (brightness_pct > BRIGHT_MAX) brightness_pct = BRIGHT_MAX;
            int mapped_b = (brightness_pct * 255) / BRIGHT_MAX;
            display.setBrightness(mapped_b);
            if (!mute && brightness_pct != prev_b)
            {
                speaker_tone_nb(
                    (brightness_pct > prev_b) ? Config::ClickUpFreq : Config::ClickDownFreq,
                    Config::ClickMs);
            }
            if (Config::DebugRot && brightness_pct != prev_b)
            {
                int delta = brightness_pct - prev_b;
                if (delta < 0) delta = -delta;
                ESP_LOGD(TAG, "[ROT] %s%d%% -> br=%d%%",
                    (brightness_pct > prev_b) ? "+" : "-", delta, brightness_pct);
            }
            draw_ring(false);
        }

        // Touch handling
        lgfx::touch_point_t tp;
        int touch_count = display.getTouch(&tp, 1);
        if (touch_count > 0)
        {
            if (!touch_active)
            {
                press_start_ms = millis();
                press_x0 = tp.x;
                press_y0 = tp.y;
                touch_dragged = false;
                touch_active = true;
                if (app_sleeping || info_overlay_active) {
                    wake_from_sleep();
                    // Swallow the wake gesture — don't let the subsequent
                    // release trigger a ping or long-press invert.
                    touch_dragged = true;
                }
                if (Config::DebugTouch)
                    ESP_LOGD(TAG, "[TOUCH] PRESS x=%d y=%d", tp.x, tp.y);
            }
            touch_last_x = tp.x;
            touch_last_y = tp.y;
            int mdx0 = tp.x - press_x0;
            int mdy0 = tp.y - press_y0;
            uint32_t move02 = (uint32_t)(mdx0 * mdx0 + mdy0 * mdy0);
            if (!touch_dragged)
            {
                uint32_t maxmove2 = (uint32_t)Config::TapMaxMovePx * (uint32_t)Config::TapMaxMovePx;
                if (move02 > maxmove2)
                {
                    touch_dragged = true;
                    if (Config::DebugTouch)
                        ESP_LOGD(TAG, "[TOUCH] DRAG start d2=%lu", (unsigned long)move02);
                }
            }
            // Update center label and crosshair
            draw_center_label();
            int txi = tp.x;
            if (txi < 0) txi = 0;
            int maxx = display.width() - 1;
            if (txi > maxx) txi = maxx;
            int tyi = tp.y;
            if (tyi < 0) tyi = 0;
            int maxy = display.height() - 1;
            if (tyi > maxy) tyi = maxy;
            draw_crosshair_overlay((int16_t)txi, (int16_t)tyi);
        }
        else if (touch_active)
        {
            uint32_t dur = millis() - press_start_ms;
            int mdx = touch_last_x - press_x0;
            int mdy = touch_last_y - press_y0;
            uint32_t move2 = (uint32_t)(mdx * mdx + mdy * mdy);
            uint32_t maxmove2 = (uint32_t)Config::TapMaxMovePx * (uint32_t)Config::TapMaxMovePx;

            if (!touch_dragged && dur > Config::LongPressInvertMs)
            {
                invert_latched = !invert_latched;
                display.invertDisplay(invert_latched);
                if (!mute) play_invert();
                if (Config::DebugTouch)
                    ESP_LOGD(TAG, "[TOUCH] RELEASE dur=%lu invert (no-drag)", (unsigned long)dur);
            }
            else if (!touch_dragged && move2 <= maxmove2)
            {
                ripple_x = touch_last_x;
                ripple_y = touch_last_y;
                ripple_prev_r = 0;
                ripple_r = 6;
                ripple_active = true;
                ripple_redraw_at = millis() + Config::PingIntervalMs;
                if (!mute) play_pop();
                if (Config::DebugTouch)
                    ESP_LOGD(TAG, "[TOUCH] RELEASE dur=%lu TAP -> ping", (unsigned long)dur);
            }
            else
            {
                draw_ring(true);
                if (Config::DebugTouch)
                    ESP_LOGD(TAG, "[TOUCH] RELEASE dur=%lu drag refresh", (unsigned long)dur);
            }
            touch_active = false;
        }

        // Target ping animation
        if (ripple_active && millis() >= ripple_redraw_at)
        {
            auto &t = THEMES[theme_idx];
            // Restore previous frame background
            if (ping_bk && ping_w > 0 && ping_h > 0)
            {
                display.pushImage(ping_x0, ping_y0, ping_w, ping_h, ping_bk);
            }
            int r = ripple_r;
            int bx0 = ripple_x - (r + 1);
            int by0 = ripple_y - (r + 1);
            int bw = (r + 1) * 2 + 1;
            int bh = (r + 1) * 2 + 1;
            if (bx0 < 0) { bw += bx0; bx0 = 0; }
            if (by0 < 0) { bh += by0; by0 = 0; }
            if (bx0 + bw > display.width())  bw = display.width() - bx0;
            if (by0 + bh > display.height()) bh = display.height() - by0;

            size_t need = (size_t)bw * (size_t)bh;
            if (need > (size_t)ping_w * (size_t)ping_h)
            {
                free(ping_bk);
                ping_bk = (uint16_t *)malloc(need * sizeof(uint16_t));
            }
            ping_w = (bw > 0 ? bw : 0);
            ping_h = (bh > 0 ? bh : 0);
            ping_x0 = bx0;
            ping_y0 = by0;
            if (ping_bk && ping_w > 0 && ping_h > 0)
            {
                display.readRect(bx0, by0, ping_w, ping_h, ping_bk);
            }
            if (ping_w > 0 && ping_h > 0)
            {
                uint16_t outline = t.ripple;
                display.drawCircle(ripple_x, ripple_y, r, outline);
                display.drawCircle(ripple_x, ripple_y, r + 1, outline);
            }
            ripple_prev_r = ripple_r;
            ripple_r += Config::PingStep;
            ripple_redraw_at = millis() + Config::PingIntervalMs;
            int maxr = std::max(display.width(), display.height());
            if (ripple_r > maxr)
            {
                if (ping_bk && ping_w > 0 && ping_h > 0)
                {
                    display.pushImage(ping_x0, ping_y0, ping_w, ping_h, ping_bk);
                }
                free(ping_bk);
                ping_bk = nullptr;
                ping_w = ping_h = 0;
                ripple_active = false;
                ripple_prev_r = 0;
                draw_ring(true);
                if (Config::DebugPing)
                    ESP_LOGD(TAG, "[PING] end");
            }
        }

        // BtnA handling
        button_update();
        if (btn_was_pressed)
        {
            if (app_sleeping || info_overlay_active) {
                wake_from_sleep();
            } else {
                theme_idx = (theme_idx + 1) % NUM_THEMES;
                draw_scene(true);
                play_confirm_up();
                if (Config::DebugBtn)
                    ESP_LOGD(TAG, "[BTN] A press -> theme %d", theme_idx + 1);
            }
        }
        if (btn_was_hold)
        {
            if (app_sleeping || info_overlay_active) {
                wake_from_sleep();
            } else {
                if (Config::DebugBtn)
                    ESP_LOGD(TAG, "[BTN] A hold -> starburst");
                effect_starburst();
            }
        }

        // Timed secondary tones (confirmation sounds)
        uint32_t now = millis();
        if (tone2_at && now >= tone2_at)
        {
            if (!mute && tone2_freq)
                speaker_tone_nb(tone2_freq, tone2_dur);
            tone2_at = 0;
            tone2_freq = 0;
            tone2_dur = 0;
        }

        // Speaker auto-stop
        speaker_update();

        // Debug heartbeat
        static uint32_t last_dbg = 0;
        if (Config::DebugHeartbeat && millis() - last_dbg > 1000)
        {
            last_dbg = millis();
            ESP_LOGD(TAG, "[DBG] br=%d%% theme=%d touch=%d drag=%d x=%d y=%d inv=%d",
                brightness_pct, theme_idx,
                (int)touch_active, (int)touch_dragged,
                cross_cx, cross_cy, (int)invert_latched);
        }

        // Yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
