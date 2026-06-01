// M5StackDial-Blinky_Claw_OpenWR — Synthwave Claude Code Status Display
// Merges M5Dial demo (display, encoder, speaker, Forth REPL) with
// Claw OpenWR (WiFi, MQTT, HTTP, NVS). Shows a fiddler crab mascot,
// Claude CLI state, model name, session/weekly usage rings, WiFi status.

#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_GC9A01.hpp>
#include <lgfx/v1/touch/Touch_FT5x06.hpp>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
// #include "mdns.h"  // mDNS removed — not reliable in multi-subnet environments
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/usb_serial_jtag.h"
#include "forth_core.h"
#include "forth_version.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>

static const char *TAG = "craw_dial";

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
#define FORTH_HEAP_SIZE     (48 * 1024)
#define NVS_NAMESPACE          "craw_config"
#define NVS_KEY_SSID           "ssid"         // Legacy — migrated to s_default
#define NVS_KEY_PASS           "pass"         // Legacy — migrated to p_default
#define NVS_KEY_MQTT_BROKER    "mqtt_url"
#define NVS_KEY_SOUND          "sound"
#define NVS_KEY_PROFILE_LIST   "prof_list"
#define NVS_KEY_ACTIVE_PROFILE "active_prof"
#define NVS_KEY_ROTATION       "display_rot"
#define HOSTNAME_PREFIX        "FiddlerCrab"
#define WIFI_MAX_RETRY         5
#define MQTT_DEFAULT_BROKER    "mqtt://broker.hivemq.com:1883"

#define PROFILE_NAME_MAX       12
#define PROFILE_MAX_COUNT      5
#define DEFAULT_PROFILE_NAME   "default"

// ---------------------------------------------------------------------------
// millis() helper
// ---------------------------------------------------------------------------
static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------------------
// USB Serial I/O
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

static void read_line_from_serial(char *buf, int maxlen, bool echo, bool allow_empty) {
    vTaskDelay(pdMS_TO_TICKS(50));
    while (1) {
        int drain = uart_getchar();
        if (drain < 0) break;
    }

    bool first_enter_skipped = false;
    int pos = 0;
    while (pos < maxlen - 1) {
        int ch = uart_getchar();
        if (ch < 0) continue;
        if (ch == '\r' || ch == '\n') {
            if (!first_enter_skipped) { first_enter_skipped = true; continue; }
            if (pos == 0 && !allow_empty) continue;
            break;
        }
        first_enter_skipped = true;
        if (ch == 8 || ch == 127) {
            if (pos > 0) { pos--; if (echo) usb_print("\b \b"); }
            continue;
        }
        if (ch >= 32) {
            buf[pos++] = (char)ch;
            if (echo) uart_putchar(ch); else uart_putchar('*');
        }
    }
    buf[pos] = '\0';
    usb_print("\r\n");
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
        auto bus_cfg        = _bus_instance.config();
        bus_cfg.spi_host    = SPI2_HOST;
        bus_cfg.freq_write  = 80000000;
        bus_cfg.pin_mosi    = 5;
        bus_cfg.pin_sclk    = 6;
        bus_cfg.pin_dc      = 4;
        _bus_instance.config(bus_cfg);
        _panel_instance.setBus(&_bus_instance);

        auto panel_cfg          = _panel_instance.config();
        panel_cfg.pin_cs        = 7;
        panel_cfg.pin_rst       = 8;
        panel_cfg.panel_width   = 240;
        panel_cfg.panel_height  = 240;
        panel_cfg.offset_rotation = 0;
        panel_cfg.invert        = true;
        _panel_instance.config(panel_cfg);

        auto light_cfg      = _light_instance.config();
        light_cfg.pin_bl    = 9;
        light_cfg.freq      = 44100;
        light_cfg.invert    = false;
        _light_instance.config(light_cfg);
        _panel_instance.setLight(&_light_instance);

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
static uint8_t speaker_volume = 180;

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

static void speaker_tone_nb(uint16_t freq, uint16_t duration_ms) {
    if (freq == 0) return;
    ledc_set_freq(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_TIMER, freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL, speaker_volume / 2);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL);
    speaker_stop_at = millis() + duration_ms;
}

static void speaker_update() {
    if (speaker_stop_at && millis() >= speaker_stop_at) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, SPEAKER_LEDC_CHANNEL);
        speaker_stop_at = 0;
    }
}

// ---------------------------------------------------------------------------
// Button A (GPIO 42, active low with pullup)
// ---------------------------------------------------------------------------
#define BTN_A_GPIO 42
static bool btn_prev = true;
static bool btn_was_pressed = false;
static uint32_t btn_press_start = 0;

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
    if (!cur && btn_prev) {
        btn_press_start = millis();
    }
    if (cur && !btn_prev) {
        uint32_t dur = millis() - btn_press_start;
        if (dur > 30) {
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
// Synthwave color palette
// ---------------------------------------------------------------------------
namespace Synth {
    static const uint16_t BG        = rgb(12, 4, 20);
    static const uint16_t CYAN      = rgb(0, 255, 255);
    static const uint16_t MAGENTA   = rgb(255, 0, 255);
    static const uint16_t HOT_PINK  = rgb(255, 105, 180);
    static const uint16_t NEON_GREEN= rgb(57, 255, 20);
    static const uint16_t YELLOW    = rgb(255, 255, 0);
    static const uint16_t RED       = rgb(255, 40, 40);
    static const uint16_t DIM_CYAN  = rgb(0, 60, 60);
    static const uint16_t DIM_GRAY  = rgb(30, 20, 40);
    static const uint16_t GRID      = rgb(40, 20, 60);
    static const uint16_t WHITE     = rgb(255, 255, 255);
    static const uint16_t TEXT_DIM  = rgb(100, 80, 120);
}

// ---------------------------------------------------------------------------
// Configuration constants
// ---------------------------------------------------------------------------
namespace Config {
    static constexpr int BrightMax  = 100;
    static constexpr int BrightStep = 10;
    static constexpr int EncDiv     = 4;

    static constexpr uint16_t ClickUpFreq   = 1800;
    static constexpr uint16_t ClickDownFreq = 1000;
    static constexpr uint16_t ClickMs       = 40;

    // Crab animation timing (RFE7)
    static constexpr uint16_t CrabLegScurryMs     = 150;   // leg phase toggle
    static constexpr uint16_t CrabClawSnapMs      = 400;   // big claw snap
    static constexpr float    CrabSmallClawSpeed   = 0.003f; // rad/ms (~2s/rev)
    static constexpr int      CrabSmallClawRadius  = 6;     // orbit pixels
    static constexpr uint16_t CrabBlinkIntervalMs  = 3000;  // between blinks
    static constexpr uint16_t CrabBlinkDurationMs  = 150;   // eyes closed
    static constexpr uint16_t CrabLookIntervalMs   = 4500;  // between looks
    static constexpr uint16_t CrabLookHoldMs       = 800;   // look hold time
    static constexpr int      CrabLookOffsetPx     = 2;     // pupil shift
    static constexpr int      CrabLegShiftPx       = 3;     // foot shift
}

// Settings view UI constants (RFE3)
namespace Settings {
    static constexpr int SoundBtnX = 40;   // top-left x
    static constexpr int SoundBtnY = 96;   // top-left y
    static constexpr int SoundBtnW = 160;  // width
    static constexpr int SoundBtnH = 56;   // height

    // Profile selector widget (RFE4)
    static constexpr int ProfBtnX = 40;
    static constexpr int ProfBtnY = 162;   // 10px gap below sound button (ends at 152)
    static constexpr int ProfBtnW = 160;
    static constexpr int ProfBtnH = 34;    // compact; fits 2 lines of size-1 text
}

// ---------------------------------------------------------------------------
// State variables
// ---------------------------------------------------------------------------

// Claw state: 0=IDLE, 2=WORKING, 3=NEED_INPUT, 5=FINISHED, 7=ERROR
static volatile int craw_state = 0;
static char model_name[32] = "---";
static volatile int session_pct = -1;     // -1=unknown, 0-100
static volatile int weekly_pct = -1;
static volatile uint32_t reset_epoch = 0;
static char client_host[32] = "";

// Dirty flags
static volatile bool dirty_status = true;
static volatile bool dirty_rings = true;
static volatile bool dirty_wifi = true;
static volatile bool dirty_full = true;

// Views (RFE3, RFE6) — button cycles App -> Settings -> Settings Rot -> App
enum View : uint8_t { VIEW_APP = 0, VIEW_SETTINGS = 1, VIEW_SETTINGS_ROT = 2 };
static View current_view = VIEW_APP;
static bool dirty_view = false;            // set on view change — wipe + redraw
static bool dirty_settings_sound = false;  // set when sound toggle needs redraw
static bool touch_was_down = false;        // rising-edge tap debounce

// Settings view: profile selector (RFE4)
static bool     dirty_settings_profile = false;  // redraw profile widget
static bool     profile_edit_mode      = false;  // true = encoder cycles profiles
static int8_t   profile_edit_index     = -1;     // index into profile_names[] while editing
static bool     profile_connecting     = false;  // brief "Connecting..." flash on save
static uint32_t profile_connect_at     = 0;      // millis() when connect was triggered

// Display rotation (RFE6) — persisted in NVS, changed via Settings screen 2
static uint8_t display_rotation = 0;      // 0..3 for 0°/90°/180°/270°, loaded from NVS at boot
static uint8_t rot_preview      = 0;      // live preview value while in VIEW_SETTINGS_ROT
static bool    dirty_rot_screen = false;  // full redraw of rotation screen

// Sound
static volatile bool sound_enabled = false;

// Session work timer — tracks active working time, pauses when waiting
static uint32_t timer_accumulated_ms = 0;  // Total working time accumulated
static uint32_t timer_segment_start = 0;   // millis() when current working segment started
static bool timer_running = false;          // true when actively counting (state == WORKING)
static volatile bool dirty_timer = true;

// Display layout
static int cx = 120, cy = 120;
static int brightness_pct = 80;
static int enc_accum = 0;

// Tone scheduling
static uint32_t tone2_at = 0;
static uint16_t tone2_freq = 0;
static uint16_t tone2_dur = 0;

// Previous state for chime detection
static int prev_craw_state = -1;

// WiFi (active profile mirror)
static char wifi_ssid[33] = {0};
static char wifi_pass[65] = {0};

// WiFi profiles
static char active_profile[PROFILE_NAME_MAX + 1] = DEFAULT_PROFILE_NAME;
static char profile_names[PROFILE_MAX_COUNT][PROFILE_NAME_MAX + 1];
static int  profile_count = 0;
static char hostname[32] = {0};
static char hostname_mdns[32] = {0};
static char mac_suffix[5] = {0};   // Last 4 hex chars of MAC, for display
static volatile bool wifi_connected = false;
static esp_netif_t *sta_netif = NULL;
static httpd_handle_t http_server = NULL;
static int wifi_retry_count = 0;
static volatile bool reconnect_pending = false;  // True during intentional reconnect

// MQTT
static char mqtt_broker_uri[128] = MQTT_DEFAULT_BROKER;
static char mqtt_topic[80] = {0};
static esp_mqtt_client_handle_t mqtt_client = NULL;
static volatile bool mqtt_connected = false;

// ---------------------------------------------------------------------------
// State name helper
// ---------------------------------------------------------------------------
static const char *state_label(int state) {
    switch (state) {
        case 0: return "IDLE";
        case 2: return "WORKING";
        case 3: return "NEED INPUT";
        case 5: return "FINISHED";
        case 7: return "ERROR";
        default: return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// Chime functions
// ---------------------------------------------------------------------------
static void chime_working() {
    if (!sound_enabled) return;
    speaker_tone_nb(800, 50);
}

static void chime_finished() {
    if (!sound_enabled) return;
    speaker_tone_nb(1200, 80);
    tone2_at = millis() + 120;
    tone2_freq = 1800;
    tone2_dur = 100;
}

static void chime_need_input() {
    if (!sound_enabled) return;
    speaker_tone_nb(2000, 100);
}

static void chime_error() {
    if (!sound_enabled) return;
    speaker_tone_nb(1000, 80);
    tone2_at = millis() + 120;
    tone2_freq = 600;
    tone2_dur = 100;
}

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------
static void nvs_init_flash(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

// ---------------------------------------------------------------------------
// WiFi Profile helpers
// ---------------------------------------------------------------------------

// Build NVS key "s_<name>" or "p_<name>" into out (min 15 bytes)
static void build_profile_key(char prefix, const char *name, char *out) {
    out[0] = prefix;
    out[1] = '_';
    int i = 0;
    while (i < PROFILE_NAME_MAX && name[i]) {
        out[i + 2] = name[i];
        i++;
    }
    out[i + 2] = '\0';
}

// Validate profile name: 1..PROFILE_NAME_MAX chars, [A-Za-z0-9_-]
static bool profile_name_valid(const char *name) {
    if (!name) return false;
    int len = (int)strlen(name);
    if (len < 1 || len > PROFILE_NAME_MAX) return false;
    for (int i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

// Lowercase a string in-place
static void str_tolower(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
    }
}

// Parse comma-separated profile list into profile_names[]
static void profile_list_parse(const char *list) {
    profile_count = 0;
    if (!list || !*list) return;
    const char *p = list;
    while (*p && profile_count < PROFILE_MAX_COUNT) {
        // Skip leading commas/whitespace
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ',' && n < PROFILE_NAME_MAX) {
            profile_names[profile_count][n++] = *p++;
        }
        profile_names[profile_count][n] = '\0';
        // Skip any overflow chars
        while (*p && *p != ',') p++;
        if (n > 0) profile_count++;
    }
}

// Serialize profile_names[] to comma-separated string, write to NVS
static void profile_list_save(void) {
    char buf[PROFILE_MAX_COUNT * (PROFILE_NAME_MAX + 1) + 4] = {0};
    int pos = 0;
    for (int i = 0; i < profile_count; i++) {
        if (i > 0 && pos < (int)sizeof(buf) - 1) buf[pos++] = ',';
        int n = (int)strlen(profile_names[i]);
        if (pos + n >= (int)sizeof(buf)) break;
        memcpy(buf + pos, profile_names[i], n);
        pos += n;
    }
    buf[pos] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_PROFILE_LIST, buf);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Find profile in profile_names[], return index or -1
static int profile_find(const char *name) {
    for (int i = 0; i < profile_count; i++) {
        if (strcmp(profile_names[i], name) == 0) return i;
    }
    return -1;
}

// Add profile to profile_names[] (memory only). Returns false if full or duplicate.
static bool profile_add(const char *name) {
    if (profile_count >= PROFILE_MAX_COUNT) return false;
    if (profile_find(name) >= 0) return false;
    strncpy(profile_names[profile_count], name, PROFILE_NAME_MAX);
    profile_names[profile_count][PROFILE_NAME_MAX] = '\0';
    profile_count++;
    return true;
}

// Remove profile from profile_names[] (memory only)
static void profile_remove(const char *name) {
    int idx = profile_find(name);
    if (idx < 0) return;
    for (int i = idx; i < profile_count - 1; i++) {
        strncpy(profile_names[i], profile_names[i + 1], PROFILE_NAME_MAX + 1);
    }
    profile_count--;
    profile_names[profile_count][0] = '\0';
}

// Load s_<name>+p_<name> into wifi_ssid/wifi_pass globals.
// Returns true if SSID was found.
static bool nvs_load_profile_creds(const char *name) {
    wifi_ssid[0] = '\0';
    wifi_pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    char key[16];
    size_t len;
    build_profile_key('s', name, key);
    len = sizeof(wifi_ssid);
    esp_err_t err = nvs_get_str(h, key, wifi_ssid, &len);
    bool ok = (err == ESP_OK && strlen(wifi_ssid) > 0);
    build_profile_key('p', name, key);
    len = sizeof(wifi_pass);
    if (nvs_get_str(h, key, wifi_pass, &len) != ESP_OK) {
        wifi_pass[0] = '\0';
    }
    nvs_close(h);
    return ok;
}

// Save wifi_ssid/wifi_pass to s_<name>+p_<name>
static void nvs_save_profile_creds(const char *name) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        usb_print("[NVS] open failed\r\n");
        return;
    }
    char key[16];
    build_profile_key('s', name, key);
    esp_err_t err = nvs_set_str(h, key, wifi_ssid);
    if (err != ESP_OK) usb_printf("[NVS] write ssid err %d\r\n", err);
    build_profile_key('p', name, key);
    err = nvs_set_str(h, key, wifi_pass);
    if (err != ESP_OK) usb_printf("[NVS] write pass err %d\r\n", err);
    nvs_commit(h);
    nvs_close(h);
}

// Erase s_<name>+p_<name> from NVS
static void nvs_erase_profile_creds(const char *name) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    build_profile_key('s', name, key);
    nvs_erase_key(h, key);
    build_profile_key('p', name, key);
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

// Write active_prof key and update in-memory active_profile
static void nvs_set_active_profile(const char *name) {
    strncpy(active_profile, name, PROFILE_NAME_MAX);
    active_profile[PROFILE_NAME_MAX] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_ACTIVE_PROFILE, active_profile);
        nvs_commit(h);
        nvs_close(h);
    }
}

// One-time migration from legacy ssid/pass schema to profile-based schema.
// Idempotent: uses active_prof key as sentinel.
static void nvs_migrate_wifi_profiles(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    // Check if already migrated
    char buf[PROFILE_NAME_MAX + 1] = {0};
    size_t len = sizeof(buf);
    if (nvs_get_str(h, NVS_KEY_ACTIVE_PROFILE, buf, &len) == ESP_OK) {
        nvs_close(h);
        return; // Already migrated
    }

    // Check for legacy ssid/pass
    char legacy_ssid[33] = {0};
    char legacy_pass[65] = {0};
    len = sizeof(legacy_ssid);
    bool has_legacy = (nvs_get_str(h, NVS_KEY_SSID, legacy_ssid, &len) == ESP_OK);
    if (has_legacy) {
        len = sizeof(legacy_pass);
        nvs_get_str(h, NVS_KEY_PASS, legacy_pass, &len);
        // Write to s_default / p_default
        nvs_set_str(h, "s_default", legacy_ssid);
        nvs_set_str(h, "p_default", legacy_pass);
        // Erase legacy keys
        nvs_erase_key(h, NVS_KEY_SSID);
        nvs_erase_key(h, NVS_KEY_PASS);
    }

    // Initialize prof_list and active_prof
    nvs_set_str(h, NVS_KEY_PROFILE_LIST, DEFAULT_PROFILE_NAME);
    nvs_set_str(h, NVS_KEY_ACTIVE_PROFILE, DEFAULT_PROFILE_NAME);
    nvs_commit(h);
    nvs_close(h);

    if (has_legacy) {
        usb_printf("[MIGRATE] Converted legacy creds to profile '%s'\r\n",
                   DEFAULT_PROFILE_NAME);
    }
}

// Load profile list and active profile from NVS into memory
static void profiles_load_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        profile_count = 0;
        strncpy(active_profile, DEFAULT_PROFILE_NAME, PROFILE_NAME_MAX);
        return;
    }
    // Load profile list
    char list_buf[PROFILE_MAX_COUNT * (PROFILE_NAME_MAX + 1) + 4] = {0};
    size_t len = sizeof(list_buf);
    if (nvs_get_str(h, NVS_KEY_PROFILE_LIST, list_buf, &len) == ESP_OK) {
        profile_list_parse(list_buf);
    }
    // Ensure default always exists in list
    if (profile_find(DEFAULT_PROFILE_NAME) < 0) {
        profile_add(DEFAULT_PROFILE_NAME);
    }
    // Load active profile
    len = sizeof(active_profile);
    if (nvs_get_str(h, NVS_KEY_ACTIVE_PROFILE, active_profile, &len) != ESP_OK) {
        strncpy(active_profile, DEFAULT_PROFILE_NAME, PROFILE_NAME_MAX);
    }
    nvs_close(h);
    // If active not in list, fall back to default
    if (profile_find(active_profile) < 0) {
        strncpy(active_profile, DEFAULT_PROFILE_NAME, PROFILE_NAME_MAX);
    }
}

// ---------------------------------------------------------------------------
// WiFi credentials (thin wrappers around profile helpers)
// ---------------------------------------------------------------------------

// Load credentials for the currently active profile into wifi_ssid/wifi_pass.
// Returns true if SSID is configured (non-empty).
static bool nvs_load_wifi_creds(void) {
    profiles_load_from_nvs();
    return nvs_load_profile_creds(active_profile);
}

// Save current wifi_ssid/wifi_pass to the active profile.
static void nvs_save_wifi_creds(void) {
    nvs_save_profile_creds(active_profile);
}

// Clear credentials for the active profile. If non-default, also remove
// the profile from the list and switch active back to default.
static void nvs_clear_wifi_creds(void) {
    bool was_default = (strcmp(active_profile, DEFAULT_PROFILE_NAME) == 0);
    char cleared_name[PROFILE_NAME_MAX + 1];
    strncpy(cleared_name, active_profile, PROFILE_NAME_MAX + 1);

    nvs_erase_profile_creds(active_profile);
    wifi_ssid[0] = '\0';
    wifi_pass[0] = '\0';

    if (!was_default) {
        profile_remove(cleared_name);
        profile_list_save();
        nvs_set_active_profile(DEFAULT_PROFILE_NAME);
        nvs_load_profile_creds(DEFAULT_PROFILE_NAME);
    }
}

static void nvs_load_mqtt_broker(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(mqtt_broker_uri);
        if (nvs_get_str(h, NVS_KEY_MQTT_BROKER, mqtt_broker_uri, &len) != ESP_OK) {
            strncpy(mqtt_broker_uri, MQTT_DEFAULT_BROKER, sizeof(mqtt_broker_uri) - 1);
        }
        nvs_close(h);
    }
}

static void nvs_save_mqtt_broker(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_MQTT_BROKER, mqtt_broker_uri);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void nvs_load_sound_pref(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(h, NVS_KEY_SOUND, &val) == ESP_OK) {
            sound_enabled = (val != 0);
        }
        nvs_close(h);
    }
}

static void nvs_save_sound_pref(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_SOUND, sound_enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Display rotation (RFE6) — persisted as u8 (0..3)
static void nvs_load_rotation(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(h, NVS_KEY_ROTATION, &val) == ESP_OK) {
            display_rotation = val & 3;
        }
        nvs_close(h);
    }
}

static void nvs_save_rotation(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_ROTATION, display_rotation);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------------------------------------------------------------------------
// Hostname
// ---------------------------------------------------------------------------
static void derive_hostname(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(hostname, sizeof(hostname), "%s_%02x%02x", HOSTNAME_PREFIX, mac[4], mac[5]);
    snprintf(hostname_mdns, sizeof(hostname_mdns), "fiddlercrab-%02x%02x", mac[4], mac[5]);
    snprintf(mac_suffix, sizeof(mac_suffix), "%02x%02x", mac[4], mac[5]);
}

// ---------------------------------------------------------------------------
// HTTP Server
// ---------------------------------------------------------------------------
static esp_err_t handler_notify(httpd_req_t *req) {
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[64] = {0};
        if (httpd_query_key_value(query, "state", param, sizeof(param)) == ESP_OK) {
            int state = atoi(param);
            craw_state = state;
            dirty_status = true;
        }
        if (httpd_query_key_value(query, "model", param, sizeof(param)) == ESP_OK) {
            strncpy(model_name, param, sizeof(model_name) - 1);
            model_name[sizeof(model_name) - 1] = '\0';
            dirty_status = true;
        }
        if (httpd_query_key_value(query, "session", param, sizeof(param)) == ESP_OK) {
            session_pct = atoi(param);
            dirty_rings = true;
        }
        if (httpd_query_key_value(query, "weekly", param, sizeof(param)) == ESP_OK) {
            weekly_pct = atoi(param);
            dirty_rings = true;
        }
        if (httpd_query_key_value(query, "host", param, sizeof(param)) == ESP_OK) {
            strncpy(client_host, param, sizeof(client_host) - 1);
            client_host[sizeof(client_host) - 1] = '\0';
            dirty_status = true;
        }
        ESP_LOGI(TAG, "HTTP notify: state=%d model=%s sess=%d wkly=%d",
                 craw_state, model_name, session_pct, weekly_pct);
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t handler_status(httpd_req_t *req) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    esp_netif_ip_info_t ip_info = {0};
    if (sta_netif) esp_netif_get_ip_info(sta_netif, &ip_info);

    char json[400];
    snprintf(json, sizeof(json),
        "{\"state\":%d,\"state_name\":\"%s\",\"model\":\"%s\","
        "\"session_pct\":%d,\"weekly_pct\":%d,"
        "\"ip\":\"" IPSTR "\","
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"hostname\":\"%s.local\",\"uptime_s\":%lu,\"free_heap\":%lu,"
        "\"sound\":%s}",
        craw_state, state_label(craw_state), model_name,
        session_pct, weekly_pct,
        IP2STR(&ip_info.ip),
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        hostname_mdns,
        (unsigned long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size(),
        sound_enabled ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t handler_ping(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "pong");
    return ESP_OK;
}

static void start_http_server(void) {
    if (http_server) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;

    if (httpd_start(&http_server, &config) == ESP_OK) {
        httpd_uri_t uri_notify = { .uri = "/notify", .method = HTTP_GET, .handler = handler_notify, .user_ctx = NULL };
        httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET, .handler = handler_status, .user_ctx = NULL };
        httpd_uri_t uri_ping   = { .uri = "/ping",   .method = HTTP_GET, .handler = handler_ping,   .user_ctx = NULL };
        httpd_register_uri_handler(http_server, &uri_notify);
        httpd_register_uri_handler(http_server, &uri_status);
        httpd_register_uri_handler(http_server, &uri_ping);
        ESP_LOGI(TAG, "HTTP server started on port 80");
    }
}

// ---------------------------------------------------------------------------
// mDNS
// ---------------------------------------------------------------------------
// mDNS removed — not reliable in multi-subnet environments
// static void start_mdns_service(void) {
//     ESP_ERROR_CHECK(mdns_init());
//     mdns_hostname_set(hostname_mdns);
//     mdns_instance_name_set("FiddlerCrab Status Display");
//     mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
//     ESP_LOGI(TAG, "mDNS: %s.local", hostname_mdns);
// }

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
static void mqtt_event_handler(void *args, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_connected = true;
            esp_mqtt_client_subscribe(mqtt_client, mqtt_topic, 1);
            ESP_LOGI(TAG, "MQTT connected, subscribed to: %s", mqtt_topic);
            usb_printf("\r\n[MQTT] Connected, topic: %s\r\n", mqtt_topic);
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            ESP_LOGW(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_DATA:
            if (event->data_len > 0 && event->data_len < 200) {
                char buf[200] = {0};
                int copy_len = event->data_len < (int)sizeof(buf) - 1 ? event->data_len : (int)sizeof(buf) - 1;
                memcpy(buf, event->data, copy_len);
                buf[copy_len] = '\0';

                // Check if pipe-delimited extended format
                if (strchr(buf, '|') != NULL) {
                    int state = 0;
                    char model[32] = {0};
                    int sess = -1;
                    int wkly = -1;
                    unsigned int reset_e = 0;
                    char host[32] = {0};
                    int parsed = sscanf(buf, "%d|%31[^|]|%d|%d|%u|%31[^|]",
                                        &state, model, &sess, &wkly, &reset_e, host);
                    if (parsed >= 1) {
                        craw_state = state;
                        dirty_status = true;
                    }
                    if (parsed >= 2 && strlen(model) > 0) {
                        strncpy(model_name, model, sizeof(model_name) - 1);
                        model_name[sizeof(model_name) - 1] = '\0';
                    }
                    if (parsed >= 3) {
                        session_pct = sess;
                        dirty_rings = true;
                    }
                    if (parsed >= 4) {
                        weekly_pct = wkly;
                        dirty_rings = true;
                    }
                    if (parsed >= 5) {
                        reset_epoch = reset_e;
                    }
                    if (parsed >= 6 && strlen(host) > 0) {
                        strncpy(client_host, host, sizeof(client_host) - 1);
                        client_host[sizeof(client_host) - 1] = '\0';
                        dirty_status = true;
                    }
                    ESP_LOGI(TAG, "MQTT: state=%d model=%s sess=%d wkly=%d host=%s",
                             craw_state, model_name, session_pct, weekly_pct, client_host);
                } else {
                    // Backward compat: plain integer
                    int state = atoi(buf);
                    craw_state = state;
                    dirty_status = true;
                    ESP_LOGI(TAG, "MQTT: state=%d (plain)", state);
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;
        default:
            break;
    }
}

static void derive_mqtt_topic(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mqtt_topic, sizeof(mqtt_topic),
             "iotj/cl/openwr/updates/%02x%02x", mac[4], mac[5]);
}

static void start_mqtt(void) {
    if (mqtt_client) return;
    derive_mqtt_topic();

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = mqtt_broker_uri;

    mqtt_client = esp_mqtt_client_init(&cfg);
    if (!mqtt_client) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return;
    }
    esp_mqtt_client_register_event(mqtt_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT started, broker: %s", mqtt_broker_uri);
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void wifi_retry_task(void *arg) {
    while (wifi_retry_count < WIFI_MAX_RETRY && !wifi_connected) {
        wifi_retry_count++;
        usb_printf("\r\n[WiFi] Retry %d/%d...\r\n", wifi_retry_count, WIFI_MAX_RETRY);
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!wifi_connected) esp_wifi_connect();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    if (!wifi_connected) {
        usb_print("\r\n[WiFi] Connect failed.\r\n");
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "WiFi disconnected, reason=%d", disc->reason);
            usb_printf("\r\n[WiFi] Disconnected (reason %d)\r\n", disc->reason);
            wifi_connected = false;
            dirty_wifi = true;
            // Skip retry task if this is an intentional reconnect (new credentials)
            if (!reconnect_pending && wifi_retry_count == 0) {
                xTaskCreate(wifi_retry_task, "wifi_retry", 3072, NULL, 3, NULL);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        wifi_retry_count = 0;
        dirty_wifi = true;

        start_http_server();
        // start_mdns_service();  // mDNS removed
        start_mqtt();
    }
}

static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    esp_netif_set_hostname(sta_netif, hostname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}

static bool wifi_started = false;

static void wifi_connect_with_creds(void) {
    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, wifi_pass, sizeof(wifi_config.sta.password) - 1);

    if (strlen(wifi_pass) == 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        wifi_config.sta.password[0] = '\0';
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    wifi_retry_count = 0;

    if (wifi_started) {
        // Already started: disconnect from current AP, apply new config, reconnect.
        // The disconnect event will NOT spawn a retry task because we set
        // wifi_connected=false and wifi_retry_count=0 here, but we pass
        // reconnect_pending to tell the handler we intended this disconnect.
        reconnect_pending = true;
        esp_wifi_disconnect();  // Fires STA_DISCONNECTED event
        vTaskDelay(pdMS_TO_TICKS(200));  // Let disconnect settle
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            usb_printf("[WiFi] connect error: %d\r\n", err);
        }
        reconnect_pending = false;
    } else {
        // First-time start
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    }
}

// ---------------------------------------------------------------------------
// Views: cycle App -> Settings -> Settings Rot -> App (RFE3 + RFE6)
// ---------------------------------------------------------------------------
static View next_view(View v) {
    switch (v) {
        case VIEW_APP:          return VIEW_SETTINGS;
        case VIEW_SETTINGS:     return VIEW_SETTINGS_ROT;
        case VIEW_SETTINGS_ROT: return VIEW_APP;
        default:                return VIEW_APP;
    }
}

static void switch_view(View v) {
    if (v == current_view) return;

    // Leaving VIEW_SETTINGS_ROT -> auto-save current preview (RFE6)
    if (current_view == VIEW_SETTINGS_ROT) {
        if (rot_preview != display_rotation) {
            display_rotation = rot_preview;
            nvs_save_rotation();
            usb_printf("[ROT] Saved rotation = %d\r\n", (int)display_rotation);
            if (sound_enabled) chime_finished();
        }
    }

    current_view = v;
    dirty_view = true;
    if (v == VIEW_APP) {
        dirty_full = true;   // reuses existing app redraw path
    } else if (v == VIEW_SETTINGS) {
        // Entering settings — reset profile edit state so stale state
        // never persists across view transitions (RFE4).
        dirty_settings_sound   = true;
        dirty_settings_profile = true;
        profile_edit_mode      = false;
        profile_edit_index     = -1;
        profile_connecting     = false;
    } else { // VIEW_SETTINGS_ROT
        // Initialize live preview to the saved rotation (RFE6)
        rot_preview = display_rotation;
        dirty_rot_screen = true;
    }
    // View-switch chirp — gated on sound_enabled (honors "sound off = silent")
    if (sound_enabled) speaker_tone_nb(1500, 40);
}

// ---------------------------------------------------------------------------
// Drawing: Synthwave background
// ---------------------------------------------------------------------------
static void draw_synthwave_bg() {
    display.fillScreen(Synth::BG);

    // Converging perspective lines from bottom center toward upper area
    int bx = 120, by = 200; // vanishing convergence near bottom center
    // Lines radiate upward and outward
    display.drawLine(bx, by, 0,   0,   Synth::GRID);
    display.drawLine(bx, by, 60,  0,   Synth::GRID);
    display.drawLine(bx, by, 120, 0,   Synth::GRID);
    display.drawLine(bx, by, 180, 0,   Synth::GRID);
    display.drawLine(bx, by, 240, 0,   Synth::GRID);
    display.drawLine(bx, by, 0,   80,  Synth::GRID);
    display.drawLine(bx, by, 240, 80,  Synth::GRID);
    display.drawLine(bx, by, 0,   160, Synth::GRID);
    display.drawLine(bx, by, 240, 160, Synth::GRID);

    // Horizontal grid lines in lower half with increasing spacing
    display.drawLine(0, 170, 240, 170, Synth::GRID);
    display.drawLine(0, 185, 240, 185, Synth::GRID);
    display.drawLine(0, 200, 240, 200, Synth::GRID);
    display.drawLine(0, 218, 240, 218, Synth::GRID);
    display.drawLine(0, 238, 240, 238, Synth::GRID);
}

// ---------------------------------------------------------------------------
// Drawing: Fiddler Crab
// ---------------------------------------------------------------------------
static void draw_crab(int ccx, int ccy) {
    uint32_t now = millis();

    // --- Clear crab bounding box ---
    display.fillRect(ccx - 32, ccy - 32, 78, 64, Synth::BG);

    // --- Compute animation parameters based on craw_state ---

    // Legs: scurry when WORKING (state 2)
    int leg_shift = 0;
    if (craw_state == 2) {
        bool leg_phase = ((now / Config::CrabLegScurryMs) % 2) == 0;
        leg_shift = leg_phase ? Config::CrabLegShiftPx : -Config::CrabLegShiftPx;
    }

    // Big claw pincers: snap when NEED INPUT (state 3)
    bool craw_open = true;
    if (craw_state == 3) {
        craw_open = ((now / Config::CrabClawSnapMs) % 2) == 0;
    }

    // Small claw tip: orbit when FINISHED (state 5)
    int small_tip_x = ccx - 30;
    int small_tip_y = ccy - 10;
    if (craw_state == 5) {
        float angle = now * Config::CrabSmallClawSpeed;
        small_tip_x = (int)(ccx - 26 + cosf(angle) * Config::CrabSmallClawRadius);
        small_tip_y = (int)(ccy - 6  + sinf(angle) * Config::CrabSmallClawRadius);
    }

    // Eyes: blink + look when IDLE (state 0)
    bool eyes_closed = false;
    int pupil_offset_x = 0;
    if (craw_state == 0) {
        uint32_t blink_cycle = now % Config::CrabBlinkIntervalMs;
        eyes_closed = (blink_cycle < Config::CrabBlinkDurationMs);
        uint32_t look_cycle = now % Config::CrabLookIntervalMs;
        if (look_cycle >= 1500 && look_cycle < (uint32_t)(1500 + Config::CrabLookHoldMs))
            pupil_offset_x = -Config::CrabLookOffsetPx;
        else if (look_cycle >= 3000 && look_cycle < (uint32_t)(3000 + Config::CrabLookHoldMs))
            pupil_offset_x = Config::CrabLookOffsetPx;
    }

    // --- Draw body (static) ---
    display.fillCircle(ccx, ccy, 16, Synth::CYAN);
    display.fillCircle(ccx - 8, ccy + 2, 12, Synth::CYAN);
    display.fillCircle(ccx + 8, ccy + 2, 12, Synth::CYAN);
    uint16_t light_cyan = rgb(100, 255, 255);
    display.drawCircle(ccx, ccy - 2, 14, light_cyan);
    display.drawLine(ccx - 12, ccy, ccx + 12, ccy, light_cyan);

    // --- Draw big claw (RIGHT) with snap animation ---
    display.fillCircle(ccx + 30, ccy - 8, 10, Synth::MAGENTA);
    display.fillCircle(ccx + 38, ccy - 14, 7, Synth::HOT_PINK);
    display.fillCircle(ccx + 24, ccy - 14, 6, Synth::MAGENTA);
    if (craw_open) {
        display.drawLine(ccx + 34, ccy - 20, ccx + 44, ccy - 30, Synth::HOT_PINK);
        display.drawLine(ccx + 30, ccy - 20, ccx + 24, ccy - 30, Synth::MAGENTA);
    } else {
        display.drawLine(ccx + 34, ccy - 20, ccx + 38, ccy - 28, Synth::HOT_PINK);
        display.drawLine(ccx + 30, ccy - 20, ccx + 30, ccy - 28, Synth::MAGENTA);
    }
    display.drawLine(ccx + 16, ccy - 2, ccx + 24, ccy - 6, Synth::CYAN);

    // --- Draw small claw (LEFT) with rotation animation ---
    display.fillCircle(ccx - 26, ccy - 6, 5, Synth::MAGENTA);
    display.fillCircle(small_tip_x, small_tip_y, 3, Synth::HOT_PINK);
    display.drawLine(ccx - 16, ccy - 2, small_tip_x, small_tip_y, Synth::CYAN);

    // --- Draw legs with scurry animation ---
    // Right legs
    display.drawLine(ccx + 12, ccy + 8,  ccx + 28 + leg_shift, ccy + 18 - leg_shift / 2, Synth::CYAN);
    display.drawLine(ccx + 10, ccy + 10, ccx + 24 + leg_shift, ccy + 24 - leg_shift / 2, Synth::CYAN);
    display.drawLine(ccx + 8,  ccy + 12, ccx + 18 + leg_shift, ccy + 28 - leg_shift / 2, Synth::CYAN);
    // Left legs (mirrored shift)
    display.drawLine(ccx - 12, ccy + 8,  ccx - 28 - leg_shift, ccy + 18 - leg_shift / 2, Synth::CYAN);
    display.drawLine(ccx - 10, ccy + 10, ccx - 24 - leg_shift, ccy + 24 - leg_shift / 2, Synth::CYAN);
    display.drawLine(ccx - 8,  ccy + 12, ccx - 18 - leg_shift, ccy + 28 - leg_shift / 2, Synth::CYAN);

    // --- Draw eyes with blink/look animation ---
    display.drawLine(ccx - 6, ccy - 14, ccx - 10, ccy - 26, Synth::CYAN);
    display.drawLine(ccx + 6, ccy - 14, ccx + 10, ccy - 26, Synth::CYAN);
    if (eyes_closed) {
        // Squint lines when blinking
        display.drawLine(ccx - 12, ccy - 27, ccx - 8, ccy - 27, Synth::DIM_CYAN);
        display.drawLine(ccx + 8,  ccy - 27, ccx + 12, ccy - 27, Synth::DIM_CYAN);
    } else {
        display.fillCircle(ccx - 10, ccy - 27, 3, Synth::NEON_GREEN);
        display.fillCircle(ccx + 10, ccy - 27, 3, Synth::NEON_GREEN);
        // Pupils with look offset
        display.fillCircle(ccx - 10 + pupil_offset_x, ccy - 28, 1, Synth::BG);
        display.fillCircle(ccx + 10 + pupil_offset_x, ccy - 28, 1, Synth::BG);
    }
}

// ---------------------------------------------------------------------------
// Drawing: Session ring (outer)
// ---------------------------------------------------------------------------
static void draw_session_ring() {
    const int ticks = 100;
    const float start_deg = -90.0f;
    const float sweep_deg = 360.0f;
    const int tick_len_major = 18;
    const int tick_len_minor = 12;
    const int major_every = 10;
    int outer_r = cx - 6;

    // Clear the ring area
    display.fillCircle(cx, cy, outer_r + 2, Synth::BG);

    // Draw all ticks dim first
    for (int i = 0; i < ticks; ++i) {
        float a = (start_deg + sweep_deg * (i / (float)ticks)) * (float)M_PI / 180.0f;
        int len = (i % major_every == 0) ? tick_len_major : tick_len_minor;
        int x1 = cx + (int)(cosf(a) * outer_r);
        int y1 = cy + (int)(sinf(a) * outer_r);
        int x0 = cx + (int)(cosf(a) * (outer_r - len));
        int y0 = cy + (int)(sinf(a) * (outer_r - len));
        display.drawLine(x0, y0, x1, y1, Synth::DIM_GRAY);
    }

    // Light up ticks based on session_pct
    if (session_pct >= 0) {
        int lit = (session_pct * ticks) / 100;
        if (lit > ticks) lit = ticks;
        for (int i = 0; i < lit; ++i) {
            float a = (start_deg + sweep_deg * (i / (float)ticks)) * (float)M_PI / 180.0f;
            int len = (i % major_every == 0) ? tick_len_major : tick_len_minor;
            int x1 = cx + (int)(cosf(a) * outer_r);
            int y1 = cy + (int)(sinf(a) * outer_r);
            int x0 = cx + (int)(cosf(a) * (outer_r - len));
            int y0 = cy + (int)(sinf(a) * (outer_r - len));
            uint16_t color;
            int pct_at = (i * 100) / ticks;
            if (pct_at <= 70) color = Synth::CYAN;
            else if (pct_at <= 90) color = Synth::YELLOW;
            else color = Synth::RED;
            display.drawLine(x0, y0, x1, y1, color);
        }
    }

    // Label at top
    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextSize(1);
    display.setTextColor(Synth::TEXT_DIM, Synth::BG);
    display.drawString("SESSION", cx, 14);

    // Percentage or "?" at upper area
    display.setTextSize(1);
    display.setTextColor(Synth::CYAN, Synth::BG);
    char sbuf[8];
    if (session_pct >= 0) {
        snprintf(sbuf, sizeof(sbuf), "%d%%", session_pct);
    } else {
        snprintf(sbuf, sizeof(sbuf), "?");
    }
    display.drawString(sbuf, cx, 26);
}

// ---------------------------------------------------------------------------
// Drawing: Weekly ring (inner)
// ---------------------------------------------------------------------------
static void draw_weekly_ring() {
    const int ticks = 50;
    const float start_deg = -90.0f;
    const float sweep_deg = 360.0f;
    const int tick_len_major = 10;
    const int tick_len_minor = 6;
    const int major_every = 5;
    // Inner ring sits inside the session ring
    int session_outer_r = cx - 6;
    int session_inner_r = session_outer_r - 18; // major tick len
    int outer_r = session_inner_r - 4;

    // Draw all ticks dim
    for (int i = 0; i < ticks; ++i) {
        float a = (start_deg + sweep_deg * (i / (float)ticks)) * (float)M_PI / 180.0f;
        int len = (i % major_every == 0) ? tick_len_major : tick_len_minor;
        int x1 = cx + (int)(cosf(a) * outer_r);
        int y1 = cy + (int)(sinf(a) * outer_r);
        int x0 = cx + (int)(cosf(a) * (outer_r - len));
        int y0 = cy + (int)(sinf(a) * (outer_r - len));
        display.drawLine(x0, y0, x1, y1, Synth::DIM_GRAY);
    }

    // Light up ticks based on weekly_pct
    if (weekly_pct >= 0) {
        int lit = (weekly_pct * ticks) / 100;
        if (lit > ticks) lit = ticks;
        for (int i = 0; i < lit; ++i) {
            float a = (start_deg + sweep_deg * (i / (float)ticks)) * (float)M_PI / 180.0f;
            int len = (i % major_every == 0) ? tick_len_major : tick_len_minor;
            int x1 = cx + (int)(cosf(a) * outer_r);
            int y1 = cy + (int)(sinf(a) * outer_r);
            int x0 = cx + (int)(cosf(a) * (outer_r - len));
            int y0 = cy + (int)(sinf(a) * (outer_r - len));
            uint16_t color;
            int pct_at = (i * 100) / ticks;
            if (pct_at <= 70) color = Synth::CYAN;
            else if (pct_at <= 90) color = Synth::YELLOW;
            else color = Synth::RED;
            display.drawLine(x0, y0, x1, y1, color);
        }
    }

    // Label at secondary position (bottom of inner ring area)
    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextSize(1);
    display.setTextColor(Synth::TEXT_DIM, Synth::BG);
    display.drawString("WEEKLY", cx, 226);

    // Percentage or "?"
    display.setTextColor(Synth::CYAN, Synth::BG);
    char wbuf[8];
    if (weekly_pct >= 0) {
        snprintf(wbuf, sizeof(wbuf), "%d%%", weekly_pct);
    } else {
        snprintf(wbuf, sizeof(wbuf), "?");
    }
    display.drawString(wbuf, cx, 216);
}

// ---------------------------------------------------------------------------
// Session timer helpers
// ---------------------------------------------------------------------------
static uint32_t timer_get_elapsed_ms() {
    uint32_t total = timer_accumulated_ms;
    if (timer_running) {
        total += millis() - timer_segment_start;
    }
    return total;
}

static void timer_start() {
    if (!timer_running) {
        timer_segment_start = millis();
        timer_running = true;
    }
}

static void timer_pause() {
    if (timer_running) {
        timer_accumulated_ms += millis() - timer_segment_start;
        timer_running = false;
    }
}

static void timer_reset() {
    timer_accumulated_ms = 0;
    timer_segment_start = millis();
    timer_running = false;
}

// ---------------------------------------------------------------------------
// Drawing: Timer ring (innermost, 3rd ring)
// ---------------------------------------------------------------------------
static void draw_timer_ring() {
    const int ticks = 60;  // 60 ticks = one per minute for a 1-hour ring
    const float start_deg = -90.0f;
    const float sweep_deg = 360.0f;
    const int tick_len_major = 8;
    const int tick_len_minor = 4;
    const int major_every = 5;  // Major tick every 5 minutes

    // Innermost ring: inside weekly ring
    int session_outer_r = cx - 6;
    int session_inner_r = session_outer_r - 18;
    int weekly_outer_r = session_inner_r - 4;
    int weekly_inner_r = weekly_outer_r - 10;
    int outer_r = weekly_inner_r - 3;

    uint32_t elapsed_ms = timer_get_elapsed_ms();
    uint32_t elapsed_s = elapsed_ms / 1000;
    uint32_t elapsed_min = elapsed_s / 60;

    // Ring represents 60 minutes (1 hour). Wraps after 60 min.
    int lit_ticks = (int)(elapsed_min % 60);

    // Draw all ticks dim
    for (int i = 0; i < ticks; ++i) {
        float a = (start_deg + sweep_deg * (i / (float)ticks)) * (float)M_PI / 180.0f;
        int len = (i % major_every == 0) ? tick_len_major : tick_len_minor;
        int x1 = cx + (int)(cosf(a) * outer_r);
        int y1 = cy + (int)(sinf(a) * outer_r);
        int x0 = cx + (int)(cosf(a) * (outer_r - len));
        int y0 = cy + (int)(sinf(a) * (outer_r - len));
        display.drawLine(x0, y0, x1, y1, Synth::DIM_GRAY);
    }

    // Light up elapsed ticks in orange/amber
    static const uint16_t TIMER_COLOR = rgb(255, 165, 0);  // Orange
    static const uint16_t TIMER_HOT   = rgb(255, 80, 0);   // Deep orange for >45 min
    for (int i = 0; i < lit_ticks && i < ticks; ++i) {
        float a = (start_deg + sweep_deg * (i / (float)ticks)) * (float)M_PI / 180.0f;
        int len = (i % major_every == 0) ? tick_len_major : tick_len_minor;
        int x1 = cx + (int)(cosf(a) * outer_r);
        int y1 = cy + (int)(sinf(a) * outer_r);
        int x0 = cx + (int)(cosf(a) * (outer_r - len));
        int y0 = cy + (int)(sinf(a) * (outer_r - len));
        display.drawLine(x0, y0, x1, y1, (i >= 45) ? TIMER_HOT : TIMER_COLOR);
    }

    // Time display: hh:mm:ss centered at bottom of inner area
    uint32_t hh = elapsed_s / 3600;
    uint32_t mm = (elapsed_s % 3600) / 60;
    uint32_t ss = elapsed_s % 60;
    char tbuf[12];
    snprintf(tbuf, sizeof(tbuf), "%02lu:%02lu:%02lu", (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);

    // Draw timer text just inside the timer ring, bottom area
    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextSize(1);

    // Pulsing color when running, dim when paused
    uint16_t timer_text_color;
    if (timer_running) {
        // Pulse between orange and bright orange
        int pulse = (millis() / 500) % 2;
        timer_text_color = pulse ? TIMER_COLOR : Synth::YELLOW;
    } else if (elapsed_s > 0) {
        timer_text_color = Synth::TEXT_DIM;  // Paused but has time
    } else {
        timer_text_color = Synth::DIM_GRAY;  // No time recorded
    }
    display.setTextColor(timer_text_color, Synth::BG);
    display.drawString(tbuf, cx, cy - 62);
}

// ---------------------------------------------------------------------------
// Drawing: Status text
// ---------------------------------------------------------------------------
static void draw_status_text() {
    display.setTextDatum(lgfx::textdatum_t::middle_center);

    // Status label above crab
    int status_y = cy - 48;
    display.setTextSize(2);

    uint16_t fg;
    uint32_t now = millis();

    switch (craw_state) {
        case 0: // IDLE
            fg = Synth::DIM_CYAN;
            break;
        case 2: { // WORKING - pulsing magenta
            float pulse = (sinf(now * 0.004f) + 1.0f) * 0.5f; // 0.0 to 1.0
            float k = 0.4f + pulse * 0.6f; // 0.4 to 1.0
            fg = dim_color(Synth::MAGENTA, k);
            break;
        }
        case 3: // NEED INPUT - alternating
            fg = ((now / 500) % 2 == 0) ? Synth::HOT_PINK : Synth::YELLOW;
            break;
        case 5: // FINISHED
            fg = Synth::NEON_GREEN;
            break;
        case 7: // ERROR - flashing
            fg = ((now / 300) % 2 == 0) ? Synth::RED : Synth::BG;
            break;
        default:
            fg = Synth::DIM_CYAN;
            break;
    }

    display.setTextColor(fg, Synth::BG);
    // Pad to fixed width to overwrite old text
    char status_buf[20];
    snprintf(status_buf, sizeof(status_buf), " %-12s", state_label(craw_state));
    display.drawString(status_buf, cx, status_y);

    // Client hostname in parens below status
    display.setTextSize(1);
    display.setTextColor(Synth::TEXT_DIM, Synth::BG);
    char host_buf[40];
    if (client_host[0] != '\0') {
        snprintf(host_buf, sizeof(host_buf), "(%s)", client_host);
    } else {
        snprintf(host_buf, sizeof(host_buf), "            ");
    }
    display.drawString(host_buf, cx, status_y + 16);

    // Model name below crab
    int model_y = cy + 42;
    display.setTextSize(1);
    display.setTextColor(Synth::TEXT_DIM, Synth::BG);
    char model_buf[36];
    snprintf(model_buf, sizeof(model_buf), " %-16s", model_name);
    display.drawString(model_buf, cx, model_y);
}

// ---------------------------------------------------------------------------
// Drawing: WiFi icon
// ---------------------------------------------------------------------------
static void draw_wifi_icon() {
    int wy = 208;
    int wx = cx;

    // Clear the full area: MAC label + WiFi icon + SSID
    display.fillRect(wx - 55, wy - 30, 110, 46, Synth::BG);

    // MAC suffix label well above WiFi arcs
    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextSize(1);
    display.setTextColor(Synth::TEXT_DIM, Synth::BG);
    display.drawString(mac_suffix, wx, wy - 26);

    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextSize(1);

    if (wifi_connected) {
        // WiFi fan: 3 concentric arcs
        for (int i = 1; i <= 3; i++) {
            int r = i * 4;
            // Draw arc segments (approximate with short lines)
            for (int a = -40; a <= 40; a += 5) {
                float rad = a * (float)M_PI / 180.0f;
                int x1 = wx + (int)(sinf(rad) * r);
                int y1 = wy - 6 - (int)(cosf(rad) * r);
                int x2 = wx + (int)(sinf((a + 5) * (float)M_PI / 180.0f) * r);
                int y2 = wy - 6 - (int)(cosf((a + 5) * (float)M_PI / 180.0f) * r);
                display.drawLine(x1, y1, x2, y2, Synth::CYAN);
            }
        }
        // Dot at base
        display.fillCircle(wx, wy - 5, 1, Synth::CYAN);

        // SSID text
        display.setTextColor(Synth::DIM_CYAN, Synth::BG);
        char ssid_short[16];
        strncpy(ssid_short, wifi_ssid, 12);
        ssid_short[12] = '\0';
        display.drawString(ssid_short, wx, wy + 8);
    } else {
        // X mark
        display.drawLine(wx - 5, wy - 8, wx + 5, wy + 2, Synth::RED);
        display.drawLine(wx + 5, wy - 8, wx - 5, wy + 2, Synth::RED);
        display.setTextColor(Synth::RED, Synth::BG);
        display.drawString("No WiFi", wx, wy + 8);
    }
}

// ---------------------------------------------------------------------------
// Full scene draw
// ---------------------------------------------------------------------------
static void draw_full_scene() {
    draw_synthwave_bg();
    draw_session_ring();
    draw_weekly_ring();
    draw_timer_ring();
    draw_crab(cx, cy);
    draw_status_text();
    draw_wifi_icon();
}

// Forward declarations for helpers defined in the profile/forth sections below
static bool profile_has_ssid(const char *name);
static const char *profile_peek_ssid(const char *name);
static int profile_find(const char *name);  // already defined earlier, but keep for safety
static void select_profile(const char *name);
static void draw_settings_profile_button();

// ---------------------------------------------------------------------------
// Drawing: Settings view (RFE3 + RFE4)
// ---------------------------------------------------------------------------
static void draw_settings_sound_button() {
    using namespace Settings;
    uint16_t border = sound_enabled ? Synth::NEON_GREEN : Synth::HOT_PINK;
    uint16_t fill   = Synth::DIM_GRAY;
    // Clear + fill
    display.fillRect(SoundBtnX, SoundBtnY, SoundBtnW, SoundBtnH, fill);
    // Double-line neon border for synthwave feel
    display.drawRect(SoundBtnX, SoundBtnY, SoundBtnW, SoundBtnH, border);
    display.drawRect(SoundBtnX + 2, SoundBtnY + 2, SoundBtnW - 4, SoundBtnH - 4, border);
    // Label
    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextColor(border, fill);
    display.setTextSize(2);
    display.drawString(sound_enabled ? "SOUND: ON" : "SOUND: OFF",
                       SoundBtnX + SoundBtnW / 2,
                       SoundBtnY + SoundBtnH / 2);
}

static void draw_settings_view() {
    draw_synthwave_bg();

    // Title
    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextColor(Synth::MAGENTA, Synth::BG);
    display.setTextSize(2);
    display.drawString("SETTINGS", cx, 40);

    // Sound toggle
    draw_settings_sound_button();

    // Profile selector (RFE4)
    draw_settings_profile_button();

    // Hint (moved to y=222 to make room for profile widget)
    display.setTextColor(Synth::TEXT_DIM, Synth::BG);
    display.setTextSize(1);
    display.drawString("Press btn to exit", cx, 222);
}

// ---------------------------------------------------------------------------
// Forth REPL task
// ---------------------------------------------------------------------------
static void forth_repl_task(void *arg) {
    (void)arg;
    forth_repl(uart_getchar, uart_putchar);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Forth FFI Words
// ---------------------------------------------------------------------------
static void w_brightness(void) {
    int n = (int)forth_pop();
    if (n < 0) n = 0;
    if (n > Config::BrightMax) n = Config::BrightMax;
    brightness_pct = n;
    int mapped = (brightness_pct * 255) / Config::BrightMax;
    display.setBrightness(mapped);
}

static void w_bright_get(void) {
    forth_push(brightness_pct);
}

static void w_invert(void) {
    static bool inv = false;
    inv = !inv;
    display.invertDisplay(inv);
}

// Display rotation Forth words (RFE6)
// Usage:  N rotation      set rotation to N (0..3)
//         rotation? .     print current rotation
static void w_rotation(void) {
    int n = (int)forth_pop();
    display_rotation = (uint8_t)(n & 3);
    rot_preview = display_rotation;
    display.setRotation(display_rotation);
    nvs_save_rotation();
    dirty_full = true;   // force app view redraw (harmless in other views)
    usb_printf("Rotation = %d\r\n", (int)display_rotation);
}

static void w_rotation_get(void) {
    forth_push((intptr_t)display_rotation);
}

// Check if a profile has an SSID configured in NVS (non-empty s_<name>)
static bool profile_has_ssid(const char *name) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    char key[16];
    build_profile_key('s', name, key);
    char tmp[33] = {0};
    size_t len = sizeof(tmp);
    bool ok = (nvs_get_str(h, key, tmp, &len) == ESP_OK && strlen(tmp) > 0);
    nvs_close(h);
    return ok;
}

// Read SSID string for a profile (returns pointer to static buf, empty if none)
static const char *profile_peek_ssid(const char *name) {
    static char tmp[33];
    tmp[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return tmp;
    char key[16];
    build_profile_key('s', name, key);
    size_t len = sizeof(tmp);
    nvs_get_str(h, key, tmp, &len);
    nvs_close(h);
    return tmp;
}

// Select a profile: set active, load creds, mark display dirty
static void select_profile(const char *name) {
    nvs_set_active_profile(name);
    nvs_load_profile_creds(name);
    dirty_wifi = true;
    if (strlen(wifi_ssid) > 0) {
        usb_printf("Selected profile '%s' (SSID: '%s')\r\n", name, wifi_ssid);
    } else {
        usb_printf("Selected profile '%s' (no SSID configured)\r\n", name);
    }
}

// ---------------------------------------------------------------------------
// Drawing: Settings profile selector (RFE4)
// ---------------------------------------------------------------------------
static void draw_settings_profile_button() {
    using namespace Settings;
    uint16_t fill = Synth::DIM_GRAY;
    uint16_t border;
    if (profile_connecting)      border = Synth::HOT_PINK;
    else if (profile_edit_mode)  border = Synth::CYAN;
    else                         border = Synth::TEXT_DIM;

    // Clear + fill
    display.fillRect(ProfBtnX, ProfBtnY, ProfBtnW, ProfBtnH, fill);
    // Double-line border for synthwave feel
    display.drawRect(ProfBtnX, ProfBtnY, ProfBtnW, ProfBtnH, border);
    display.drawRect(ProfBtnX + 2, ProfBtnY + 2, ProfBtnW - 4, ProfBtnH - 4, border);

    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextSize(1);
    int cxw = ProfBtnX + ProfBtnW / 2;
    int cyw = ProfBtnY + ProfBtnH / 2;

    if (profile_connecting) {
        display.setTextColor(Synth::HOT_PINK, fill);
        display.drawString("Connecting...", cxw, cyw);
        return;
    }

    if (!profile_edit_mode) {
        // View mode: show "PROFILE" / <active>
        display.setTextColor(Synth::MAGENTA, fill);
        display.drawString("PROFILE", cxw, cyw - 7);
        display.setTextColor(Synth::NEON_GREEN, fill);
        display.drawString(active_profile, cxw, cyw + 7);
    } else {
        // Edit mode: show "<name>  i/N" and SSID preview
        const char *name = (profile_edit_index >= 0 && profile_edit_index < profile_count)
                               ? profile_names[profile_edit_index] : "?";
        char line1[24];
        snprintf(line1, sizeof(line1), "%s  %d/%d",
                 name, (int)profile_edit_index + 1, (int)profile_count);
        display.setTextColor(Synth::CYAN, fill);
        display.drawString(line1, cxw, cyw - 7);

        const char *ssid = profile_peek_ssid(name);
        char line2[24];
        if (ssid && ssid[0]) {
            snprintf(line2, sizeof(line2), "%.19s", ssid);
        } else {
            snprintf(line2, sizeof(line2), "(no creds)");
        }
        display.setTextColor(Synth::TEXT_DIM, fill);
        display.drawString(line2, cxw, cyw + 7);
    }
}

// ---------------------------------------------------------------------------
// Drawing: Settings screen 2 — display rotation (RFE6)
// ---------------------------------------------------------------------------
static void draw_settings_rot_view() {
    draw_synthwave_bg();

    // Title
    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.setTextColor(Synth::MAGENTA, Synth::BG);
    display.setTextSize(2);
    display.drawString("ROTATION", cx, 40);

    // Big degree display in the center
    static const char *labels[4] = { "0", "90", "180", "270" };
    display.setTextColor(Synth::CYAN, Synth::BG);
    display.setTextSize(5);
    display.drawString(labels[rot_preview & 3], cx, cy);

    // Degree unit
    display.setTextColor(Synth::NEON_GREEN, Synth::BG);
    display.setTextSize(2);
    display.drawString("degrees", cx, cy + 40);

    // Instructions
    display.setTextColor(Synth::DIM_CYAN, Synth::BG);
    display.setTextSize(1);
    display.drawString("Rotate to adjust", cx, 200);

    // Save hint
    display.setTextColor(Synth::TEXT_DIM, Synth::BG);
    display.drawString("Press btn to save & exit", cx, 222);
}

static void w_wifi_ssid(void) {
    usb_printf("Active profile: '%s'\r\n", active_profile);
    usb_print("SSID: ");
    read_line_from_serial(wifi_ssid, sizeof(wifi_ssid), true, false);
    usb_printf("SSID set to: '%s'\r\n", wifi_ssid);
}

static void w_wifi_pass(void) {
    usb_printf("Active profile: '%s'\r\n", active_profile);
    usb_print("Password (Enter for open): ");
    read_line_from_serial(wifi_pass, sizeof(wifi_pass), false, true);
    if (strlen(wifi_pass) == 0)
        usb_print("No password (open network)\r\n");
    else
        usb_print("Password set.\r\n");
}

static void w_wifi_connect(void) {
    if (strlen(wifi_ssid) == 0) {
        usb_print("No SSID set. Use wifi-ssid first.\r\n");
        return;
    }
    nvs_save_wifi_creds();
    usb_printf("Saved to profile '%s'. Connecting to '%s'...\r\n",
               active_profile, wifi_ssid);
    wifi_connect_with_creds();
}

static void w_wifi_status(void) {
    usb_printf("Profile:    '%s' (%d of %d)\r\n",
               active_profile, profile_find(active_profile) + 1, profile_count);
    // List all profiles with markers
    usb_print("All:        ");
    for (int i = 0; i < profile_count; i++) {
        if (i > 0) usb_print(", ");
        usb_print(profile_names[i]);
        if (strcmp(profile_names[i], active_profile) == 0) usb_print("*");
        if (profile_has_ssid(profile_names[i])) {
            usb_print(" [x]");
        } else {
            usb_print(" [-]");
        }
    }
    usb_print("\r\n");

    usb_printf("SSID:       '%s'\r\n", wifi_ssid);
    usb_printf("Connected:  %s\r\n", wifi_connected ? "yes" : "no");
    if (wifi_connected && sta_netif) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(sta_netif, &ip_info);
        usb_printf("IP:         " IPSTR "\r\n", IP2STR(&ip_info.ip));
    }
    usb_printf("Hostname:   %s.local\r\n", hostname_mdns);
    usb_printf("Free heap:  %lu bytes\r\n", (unsigned long)esp_get_free_heap_size());
}

static void w_wifi_clear(void) {
    char cleared[PROFILE_NAME_MAX + 1];
    strncpy(cleared, active_profile, PROFILE_NAME_MAX + 1);
    bool was_default = (strcmp(cleared, DEFAULT_PROFILE_NAME) == 0);
    nvs_clear_wifi_creds();
    dirty_wifi = true;
    if (was_default) {
        usb_print("Credentials cleared for profile 'default'.\r\n");
    } else {
        usb_printf("Credentials cleared for profile '%s'. Profile removed. Active profile is now 'default'.\r\n",
                   cleared);
    }
}

static void w_wifi_profiles(void) {
    // List all profiles with markers
    usb_print("WiFi Profiles:\r\n");
    for (int i = 0; i < profile_count; i++) {
        char line[96];
        const char *ssid = profile_peek_ssid(profile_names[i]);
        bool is_active = (strcmp(profile_names[i], active_profile) == 0);
        if (strlen(ssid) > 0) {
            snprintf(line, sizeof(line), "  %d) %-12s %s  [ssid: %s]\r\n",
                     i + 1, profile_names[i], is_active ? "[*]" : "   ", ssid);
        } else {
            snprintf(line, sizeof(line), "  %d) %-12s %s  [no creds]\r\n",
                     i + 1, profile_names[i], is_active ? "[*]" : "   ");
        }
        usb_print(line);
    }
    usb_printf("  (%d/%d profiles)\r\n\r\n", profile_count, PROFILE_MAX_COUNT);
    usb_print("Enter number to select, name to select/create, or blank to cancel:\r\n");

    char input[PROFILE_NAME_MAX + 4] = {0};
    read_line_from_serial(input, sizeof(input), true, true);

    if (strlen(input) == 0) {
        usb_print("Cancelled.\r\n");
        return;
    }

    // Check if it's numeric
    bool is_num = true;
    for (int i = 0; input[i]; i++) {
        if (input[i] < '0' || input[i] > '9') { is_num = false; break; }
    }

    if (is_num) {
        int n = atoi(input);
        if (n < 1 || n > profile_count) {
            usb_print("No such profile number.\r\n");
            return;
        }
        select_profile(profile_names[n - 1]);
        return;
    }

    // Name: normalize to lowercase and validate
    str_tolower(input);
    if (!profile_name_valid(input)) {
        usb_print("Invalid profile name. Use 1-12 chars: letters, digits, - or _.\r\n");
        return;
    }

    // Existing profile?
    if (profile_find(input) >= 0) {
        select_profile(input);
        return;
    }

    // New profile
    if (profile_count >= PROFILE_MAX_COUNT) {
        usb_printf("Maximum profiles (%d) reached. Use wifi-clear on a non-default profile first.\r\n",
                   PROFILE_MAX_COUNT);
        return;
    }
    if (!profile_add(input)) {
        usb_print("Failed to add profile.\r\n");
        return;
    }
    profile_list_save();
    select_profile(input);
    usb_print("Created new profile. Use wifi-ssid / wifi-pass / wifi-connect to configure.\r\n");
}

static void w_mqtt_broker(void) {
    usb_printf("Current: %s\r\n", mqtt_broker_uri);
    usb_print("New broker URI (Enter to keep): ");
    char buf[128] = {0};
    read_line_from_serial(buf, sizeof(buf), true, true);
    if (strlen(buf) > 0) {
        strncpy(mqtt_broker_uri, buf, sizeof(mqtt_broker_uri) - 1);
        nvs_save_mqtt_broker();
        usb_printf("Broker set to: %s\r\n", mqtt_broker_uri);
        if (mqtt_client) {
            esp_mqtt_client_stop(mqtt_client);
            esp_mqtt_client_destroy(mqtt_client);
            mqtt_client = NULL;
            mqtt_connected = false;
            if (wifi_connected) start_mqtt();
        }
    } else {
        usb_print("Unchanged.\r\n");
    }
}

static void w_mqtt_status(void) {
    usb_printf("Broker:    %s\r\n", mqtt_broker_uri);
    usb_printf("Topic:     %s\r\n", mqtt_topic[0] ? mqtt_topic : "(not set)");
    usb_printf("Connected: %s\r\n", mqtt_connected ? "yes" : "no");
}

static void w_mqtt_topic(void) {
    if (mqtt_topic[0] == '\0') derive_mqtt_topic();
    usb_printf("%s\r\n", mqtt_topic);
}

static void w_sound_on(void) {
    sound_enabled = true;
    nvs_save_sound_pref();
    usb_print("Sound enabled.\r\n");
    speaker_tone_nb(1200, 60);
}

static void w_sound_off(void) {
    sound_enabled = false;
    nvs_save_sound_pref();
    usb_print("Sound disabled.\r\n");
}

static void w_status(void) {
    usb_printf("State:   %d (%s)\r\n", craw_state, state_label(craw_state));
    usb_printf("Model:   %s\r\n", model_name);
    usb_printf("Session: %d%%\r\n", session_pct);
    usb_printf("Weekly:  %d%%\r\n", weekly_pct);
    usb_printf("Client:  %s\r\n", client_host[0] ? client_host : "(none)");
    usb_printf("Sound:   %s\r\n", sound_enabled ? "on" : "off");
    usb_printf("Bright:  %d%%\r\n", brightness_pct);
}

static void register_forth_words(void) {
    forth_register_word("brightness", w_brightness);
    forth_register_word("bright?",    w_bright_get);
    forth_register_word("invert",     w_invert);
    forth_register_word("rotation",   w_rotation);
    forth_register_word("rotation?",  w_rotation_get);
    forth_register_word("wifi-ssid",    w_wifi_ssid);
    forth_register_word("wifi-pass",    w_wifi_pass);
    forth_register_word("wifi-connect", w_wifi_connect);
    forth_register_word("wifi-status",  w_wifi_status);
    forth_register_word("wifi-clear",   w_wifi_clear);
    forth_register_word("wifi-profiles", w_wifi_profiles);
    forth_register_word("mqtt-broker",  w_mqtt_broker);
    forth_register_word("mqtt-status",  w_mqtt_status);
    forth_register_word("mqtt-topic",   w_mqtt_topic);
    forth_register_word("sound-on",     w_sound_on);
    forth_register_word("sound-off",    w_sound_off);
    forth_register_word("status",       w_status);
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
extern "C" void app_main(void)
{
    // 1. USB serial
    setup_usb_serial();
    vTaskDelay(pdMS_TO_TICKS(500));

    // 2. Banner
    usb_print("\r\n\r\n");
    usb_print("============================================\r\n");
    usb_printf("  FiddlerCrab Display v%s\r\n", ESPIDFORTH_VERSION_STRING);
    usb_printf("  Build: %s %s\r\n", ESPIDFORTH_BUILD_DATE, ESPIDFORTH_BUILD_TIME);
    usb_print("  Synthwave Claude Code Status Display\r\n");
    usb_print("============================================\r\n");

    // 3. Hardware init
    display.init();
    encoder_init();
    speaker_init();
    button_init();
    nvs_init_flash();
    nvs_migrate_wifi_profiles();

    // Load and apply saved display rotation (RFE6)
    nvs_load_rotation();
    display.setRotation(display_rotation);
    rot_preview = display_rotation;
    usb_printf("Display rotation = %d\r\n", (int)display_rotation);

    usb_print("Hardware initialized.\r\n");

    // 4. Layout
    cx = display.width() / 2;
    cy = display.height() / 2;

    // Set initial brightness
    int mapped = (brightness_pct * 255) / Config::BrightMax;
    display.setBrightness(mapped);

    // 5. Derive hostname BEFORE drawing (need mac_suffix for display)
    derive_hostname();
    usb_printf("Hostname: %s.local\r\n", hostname_mdns);
    usb_printf("MAC suffix: %s\r\n", mac_suffix);

    // 6. Initial draw
    draw_full_scene();

    wifi_init_sta();

    nvs_load_mqtt_broker();
    usb_printf("MQTT broker: %s\r\n", mqtt_broker_uri);

    // Load sound preference
    nvs_load_sound_pref();
    usb_printf("Sound: %s\r\n", sound_enabled ? "on" : "off");

    // 7. Forth init + register words
    forth_init(FORTH_HEAP_SIZE);
    register_forth_words();
    usb_print("Forth engine initialized.\r\n");
    usb_printf("Free heap: %lu bytes\r\n\r\n",
        (unsigned long)esp_get_free_heap_size());

    usb_print("Forth commands:\r\n");
    usb_print("  N brightness  -- set brightness (0-100)\r\n");
    usb_print("  bright?       -- show current brightness\r\n");
    usb_print("  invert        -- toggle display inversion\r\n");
    usb_print("  wifi-ssid     -- set WiFi SSID\r\n");
    usb_print("  wifi-pass     -- set WiFi password\r\n");
    usb_print("  wifi-connect  -- connect and save\r\n");
    usb_print("  wifi-status   -- show connection info\r\n");
    usb_print("  wifi-clear    -- erase credentials\r\n");
    usb_print("  mqtt-broker   -- view/change MQTT broker\r\n");
    usb_print("  mqtt-status   -- show MQTT state\r\n");
    usb_print("  mqtt-topic    -- print subscription topic\r\n");
    usb_print("  sound-on      -- enable chimes\r\n");
    usb_print("  sound-off     -- disable chimes\r\n");
    usb_print("  status        -- print current state/model/usage\r\n\r\n");

    // 8. Start Forth REPL task
    xTaskCreate(forth_repl_task, "forth_repl", 8192, NULL, 3, NULL);

    // 9. Auto-connect WiFi from NVS
    if (nvs_load_wifi_creds()) {
        usb_printf("Stored WiFi: '%s' -- connecting...\r\n", wifi_ssid);
        wifi_connect_with_creds();
    } else {
        usb_print("No stored WiFi. Configure via REPL.\r\n");
    }

    // Read and discard any initial encoder counts from boot
    encoder_read_and_reset();

    // 10. Main render loop
    while (1)
    {
        uint32_t now = millis();

        // --- Check state changes, play chimes, manage timer, set dirty flags ---
        if (craw_state != prev_craw_state) {
            switch (craw_state) {
                case 2: chime_working();    break;
                case 3: chime_need_input(); break;
                case 5: chime_finished();   break;
                case 7: chime_error();      break;
                default: break;
            }

            // Timer management
            switch (craw_state) {
                case 2:  // WORKING — start or resume timer
                    if (prev_craw_state == 0 || prev_craw_state == 5 || prev_craw_state == -1) {
                        timer_reset();  // New session, reset
                    }
                    timer_start();
                    break;
                case 3:  // NEED INPUT — pause timer
                case 0:  // IDLE — pause timer
                    timer_pause();
                    break;
                case 5:  // FINISHED — pause timer (keep time visible)
                    timer_pause();
                    break;
                case 7:  // ERROR — pause timer
                    timer_pause();
                    break;
            }
            dirty_timer = true;

            prev_craw_state = craw_state;
            dirty_status = true;
        }

        // --- View switch: full wipe + redraw for the new view (RFE3 + RFE6) ---
        if (dirty_view) {
            display.fillScreen(Synth::BG);
            if (current_view == VIEW_SETTINGS) {
                draw_settings_view();
                dirty_settings_sound = false;
                dirty_settings_profile = false;
                touch_was_down = false; // fresh rising-edge state on entry
            } else if (current_view == VIEW_SETTINGS_ROT) {
                draw_settings_rot_view();
                dirty_rot_screen = false;
                touch_was_down = false;
            }
            // VIEW_APP case: dirty_full was already set by switch_view(),
            // the app pipeline below will handle the redraw.
            dirty_view = false;
        }

        // ============================================================
        // App view draw pipeline — gated on current_view == VIEW_APP
        // ============================================================
        if (current_view == VIEW_APP) {
            // --- Full redraw if needed ---
            if (dirty_full) {
                draw_full_scene();
                dirty_full = false;
                dirty_status = false;
                dirty_rings = false;
                dirty_wifi = false;
                dirty_timer = false;
            }

            // --- Partial redraws ---
            if (dirty_rings) {
                draw_session_ring();
                draw_weekly_ring();
                draw_timer_ring();
                draw_crab(cx, cy); // Redraw crab since rings clear center
                draw_wifi_icon();  // Redraw WiFi since fillCircle clears everything
                dirty_rings = false;
                dirty_timer = false;
                dirty_wifi = false;
                dirty_status = true; // Need to redraw text on top
            }

            // --- Timer ring: update every second while running ---
            if (timer_running) {
                static uint32_t last_timer_draw = 0;
                if (now - last_timer_draw >= 1000) {
                    draw_timer_ring();
                    last_timer_draw = now;
                }
            } else if (dirty_timer) {
                draw_timer_ring();
                dirty_timer = false;
            }

            if (dirty_wifi) {
                draw_wifi_icon();
                dirty_wifi = false;
            }

            // --- Crab animation (RFE7): redraw crab with state-dependent motion ---
            {
                bool needs_crab_anim = (craw_state == 2 || craw_state == 3 || craw_state == 5);
                bool needs_idle_anim = (craw_state == 0);
                static uint32_t last_idle_crab_draw = 0;
                if (needs_crab_anim) {
                    draw_crab(cx, cy);
                } else if (needs_idle_anim && (now - last_idle_crab_draw >= 33)) {
                    draw_crab(cx, cy);
                    last_idle_crab_draw = now;
                }
            }

            // --- Status text animation ---
            bool needs_text_anim = (craw_state == 2 || craw_state == 3 || craw_state == 7);
            if (dirty_status || needs_text_anim) {
                draw_status_text();
                dirty_status = false;
            }
        }
        // ============================================================
        // Settings view draw pipeline (RFE3 + RFE4)
        // ============================================================
        else if (current_view == VIEW_SETTINGS) {
            if (dirty_settings_sound) {
                draw_settings_sound_button();
                dirty_settings_sound = false;
            }

            // Auto-clear "Connecting..." flash after ~1500ms (RFE4)
            if (profile_connecting && (millis() - profile_connect_at) > 1500) {
                profile_connecting = false;
                dirty_settings_profile = true;
            }
            if (dirty_settings_profile) {
                draw_settings_profile_button();
                dirty_settings_profile = false;
            }

            // Touch polling for settings view
            lgfx::touch_point_t tp;
            int tn = display.getTouch(&tp, 1);
            bool down = (tn > 0);
            if (down && !touch_was_down) {
                using namespace Settings;
                // Sound toggle
                if (tp.x >= SoundBtnX && tp.x < SoundBtnX + SoundBtnW &&
                    tp.y >= SoundBtnY && tp.y < SoundBtnY + SoundBtnH) {
                    bool prev = sound_enabled;
                    sound_enabled = !sound_enabled;
                    nvs_save_sound_pref();
                    dirty_settings_sound = true;
                    usb_printf("[TOUCH] Sound %s\r\n", sound_enabled ? "ON" : "OFF");
                    // Confirmation beep only when toggling from OFF -> ON
                    if (sound_enabled && !prev) speaker_tone_nb(1200, 60);
                }
                // Profile widget (RFE4)
                else if (tp.x >= ProfBtnX && tp.x < ProfBtnX + ProfBtnW &&
                         tp.y >= ProfBtnY && tp.y < ProfBtnY + ProfBtnH) {
                    if (profile_connecting) {
                        // Ignore taps during connecting flash
                    } else if (!profile_edit_mode) {
                        // Enter edit mode
                        if (profile_count <= 0) {
                            if (sound_enabled) chime_error();
                        } else {
                            int idx = profile_find(active_profile);
                            if (idx < 0) idx = 0;
                            profile_edit_index = (int8_t)idx;
                            profile_edit_mode = true;
                            dirty_settings_profile = true;
                            if (sound_enabled) chime_need_input();
                            usb_printf("[TOUCH] Profile edit: idx=%d name=%s\r\n",
                                       idx, profile_names[idx]);
                        }
                    } else {
                        // Save / commit
                        const char *chosen = profile_names[profile_edit_index];
                        bool changed = (strcmp(chosen, active_profile) != 0);
                        profile_edit_mode = false;
                        if (changed) {
                            select_profile(chosen);
                            if (profile_has_ssid(chosen)) {
                                wifi_connect_with_creds();
                                profile_connecting = true;
                                profile_connect_at = millis();
                                if (sound_enabled) chime_working();
                                usb_printf("[TOUCH] Profile saved: %s (connecting)\r\n", chosen);
                            } else {
                                if (sound_enabled) chime_error();
                                usb_printf("[TOUCH] Profile saved: %s (no creds, not connecting)\r\n", chosen);
                            }
                        } else {
                            if (sound_enabled) chime_finished();
                            usb_printf("[TOUCH] Profile unchanged: %s\r\n", chosen);
                        }
                        dirty_settings_profile = true;
                    }
                }
            }
            touch_was_down = down;
        }
        // ============================================================
        // Rotation view draw pipeline (RFE6)
        // ============================================================
        else { // VIEW_SETTINGS_ROT
            if (dirty_rot_screen) {
                draw_settings_rot_view();
                dirty_rot_screen = false;
            }
            // No touch handling — only encoder + button drive this view
        }

        // --- Encoder -> brightness ---
        int32_t d = encoder_read_and_reset();
        if (d)
            enc_accum += d;
        int logical = 0;
        while (enc_accum >= Config::EncDiv) {
            enc_accum -= Config::EncDiv;
            ++logical;
        }
        while (enc_accum <= -Config::EncDiv) {
            enc_accum += Config::EncDiv;
            --logical;
        }
        if (logical) {
            // In rotation view, encoder live-previews display rotation (RFE6)
            if (current_view == VIEW_SETTINGS_ROT) {
                uint8_t prev = rot_preview;
                int nxt = ((int)rot_preview + logical) & 3;
                rot_preview = (uint8_t)nxt;
                if (prev != rot_preview) {
                    display.setRotation(rot_preview);   // LIVE apply
                    dirty_rot_screen = true;            // full redraw in new orientation
                    if (sound_enabled) {
                        speaker_tone_nb(
                            (logical > 0) ? Config::ClickUpFreq : Config::ClickDownFreq,
                            Config::ClickMs);
                    }
                }
            }
            // In profile edit mode, encoder cycles through profiles instead of brightness (RFE4)
            else if (current_view == VIEW_SETTINGS && profile_edit_mode && profile_count > 0) {
                int prev_idx = profile_edit_index;
                int n = (int)profile_count;
                int idx = ((int)profile_edit_index + logical) % n;
                if (idx < 0) idx += n;
                profile_edit_index = (int8_t)idx;
                if (prev_idx != idx) {
                    dirty_settings_profile = true;
                    if (sound_enabled) {
                        speaker_tone_nb(
                            (logical > 0) ? Config::ClickUpFreq : Config::ClickDownFreq,
                            Config::ClickMs);
                    }
                }
            } else {
                // Normal: encoder adjusts brightness
                int prev_b = brightness_pct;
                brightness_pct += logical * Config::BrightStep;
                if (brightness_pct < 0) brightness_pct = 0;
                if (brightness_pct > Config::BrightMax) brightness_pct = Config::BrightMax;
                int mapped_b = (brightness_pct * 255) / Config::BrightMax;
                display.setBrightness(mapped_b);
                if (sound_enabled && brightness_pct != prev_b) {
                    speaker_tone_nb(
                        (brightness_pct > prev_b) ? Config::ClickUpFreq : Config::ClickDownFreq,
                        Config::ClickMs);
                }
            }
        }

        // --- Button -> cancel edit / cycle view (RFE3 + RFE4 + RFE6) ---
        // In profile edit mode, button cancels edit without leaving settings view.
        // Otherwise, short press cycles: App -> Settings -> Settings Rot -> App.
        button_update();
        if (btn_was_pressed) {
            if (current_view == VIEW_SETTINGS && profile_edit_mode) {
                // Cancel edit, stay in settings view
                profile_edit_mode = false;
                profile_edit_index = -1;
                dirty_settings_profile = true;
                if (sound_enabled) chime_error();
                usb_print("[BTN] Profile edit cancelled\r\n");
            } else {
                View v = next_view(current_view);
                switch_view(v);
                const char *name = (v == VIEW_APP)      ? "APP"
                                  : (v == VIEW_SETTINGS) ? "SETTINGS"
                                                         : "SETTINGS_ROT";
                usb_printf("[BTN] View = %s\r\n", name);
            }
        }

        // --- Tone2 scheduling ---
        if (tone2_at && now >= tone2_at) {
            if (sound_enabled && tone2_freq)
                speaker_tone_nb(tone2_freq, tone2_dur);
            tone2_at = 0;
            tone2_freq = 0;
            tone2_dur = 0;
        }

        // --- Speaker auto-stop ---
        speaker_update();

        // --- Yield ---
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
