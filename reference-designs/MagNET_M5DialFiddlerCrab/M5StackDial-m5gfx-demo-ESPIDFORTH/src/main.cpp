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
// Drawing functions
// ---------------------------------------------------------------------------
static void draw_status()
{
    (void)THEMES;
    (void)brightness_pct;
    (void)invert_latched;
    (void)mute;
}

static void draw_center_label()
{
    auto &t = THEMES[theme_idx];
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

static void register_forth_words(void) {
    forth_register_word("theme",       w_theme);
    forth_register_word("brightness",  w_brightness);
    forth_register_word("ping",        w_ping);
    forth_register_word("starburst",   w_starburst);
    forth_register_word("invert",      w_invert);
    forth_register_word("mute",        w_mute);
    forth_register_word("theme?",      w_theme_get);
    forth_register_word("bright?",     w_bright_get);
    forth_register_word("appbeep",     w_appbeep);
    forth_register_word("appsleep",    w_appsleep);
    forth_register_word("appshowmem",  w_appshowmem);
    forth_register_word("appdevinfo",  w_appdevinfo);
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
    usb_print("  appdevinfo   -- display chip / CPU info\r\n\r\n");

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
