// M5StickCPlus-Blinky_Voice_XR — ROBOTALK XR
// Synthwave voice/IMU node on M5StickC Plus (240x135 landscape ST7789, ESP32).
//
// Forked from M5StickCPlus-Blinky_Crawdad_OpenWR. No longer tracks Claude Code
// sessions. Instead:
//   - MAIN screen: "ROBOTALK XR" with an animated robot face that fidgets
//     occasionally while WiFi is connected.
//   - CONVO screen: PDM-mic capture (long-press B to record) + live MPU6886
//     IMU readout with flat-on-table calibration.
//   - SETTINGS screen: unchanged layout.
//   - Publishes live IMU telemetry over MQTT to iotj/axrawe/updates/<DEVICEID>.
//
// Uses shared craw_* components for WiFi, NVS, MQTT, HTTP, speaker, serial,
// plus board-local craw_mic (SPM1423 PDM) and craw_imu_mpu6886.
// Includes ESPIDFORTH REPL for configuration.

#include <M5GFX.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

#include "craw_serial.h"
#include "craw_speaker.h"
#include "craw_wifi.h"
#include "craw_nvs.h"
#include "craw_mqtt.h"
#include "craw_http.h"
#include "craw_mic.h"
#include "craw_imu_mpu6886.h"
#include "forth_core.h"
#include "forth_version.h"

#include <cstdio>
#include <cstring>
#include <cmath>

static const char *TAG = "robotalk";

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
#define FORTH_HEAP_SIZE      (32 * 1024)
#define HOSTNAME_PREFIX      "RobotalkXR"
#define VERSION_STRING       "0.1.0"

#define BUTTON_A_GPIO        GPIO_NUM_37
#define BUTTON_B_GPIO        GPIO_NUM_39
#define SPEAKER_GPIO         2

#define SCREEN_W             240
#define SCREEN_H             135

// Long-press thresholds (ms)
#define LONGPRESS_RECORD_MS  800     // B held on CONVO -> start recording
#define LONGPRESS_WIFI_MS    3000    // B held on SETTINGS -> clear WiFi

// Mic capture target. Buffer is sized against free internal DRAM at record
// time; this caps it so we never grab the whole heap. See compute_record_secs().
#define MIC_MAX_RECORD_SECS  4

// IMU MQTT publish cadence
#define IMU_PUBLISH_INTERVAL_MS  100  // ~10 Hz

// ---------------------------------------------------------------------------
// millis() helper
// ---------------------------------------------------------------------------
static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------------------
// Synthwave color palette (RGB565)
// ---------------------------------------------------------------------------
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

namespace Synth {
    static const uint16_t BG         = rgb(12, 4, 20);
    static const uint16_t CYAN       = rgb(0, 255, 255);
    static const uint16_t MAGENTA    = rgb(255, 0, 255);
    static const uint16_t HOT_PINK   = rgb(255, 105, 180);
    static const uint16_t NEON_GREEN = rgb(57, 255, 20);
    static const uint16_t YELLOW     = rgb(255, 255, 0);
    static const uint16_t RED        = rgb(255, 40, 40);
    static const uint16_t DIM_CYAN   = rgb(0, 60, 60);
    static const uint16_t DIM_GRAY   = rgb(30, 20, 40);
    static const uint16_t GRID       = rgb(40, 20, 60);
    static const uint16_t WHITE      = rgb(255, 255, 255);
    static const uint16_t TEXT_DIM   = rgb(100, 80, 120);
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
static M5GFX display;
static M5Canvas canvas(&display);  // off-screen sprite for flicker-free drawing

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
enum screen_t { SCREEN_WIFI = 0, SCREEN_CONVO, SCREEN_SETTINGS };
static screen_t current_screen = SCREEN_WIFI;

static char hostname[32]    = {0};
static char mac_suffix[8]   = {0};
static char mqtt_topic[128] = {0};
static char mqtt_broker[128] = {0};
static char wifi_ssid[33]   = {0};
static char wifi_pass[65]   = {0};

static int  brightness_pct  = 80;
static bool sound_enabled   = true;

// Dirty flag for redraw
static volatile bool dirty_screen = true;

// Settings screen
static int  settings_cursor = 0;
#define SETTINGS_ITEMS 5  // Sound, Bright, Profile, Clear(cal), Back

// Button state
static bool btn_a_last = true;   // active low, true = released
static bool btn_b_last = true;
static uint32_t btn_b_press_time = 0;
static bool btn_b_long_fired = false;

// Robot face animation
static bool     robot_blink     = false;
static int      robot_look_dx   = 0;
static uint32_t robot_next_blink = 0;
static uint32_t robot_blink_until = 0;
static uint32_t robot_next_look  = 0;
static uint32_t robot_look_until = 0;

// Recording state (CONVO)
static volatile bool  recording        = false;
static volatile float rec_fraction     = 0.0f;
static volatile float last_capture_secs = 0.0f;
static volatile int   last_capture_peak = 0;
static volatile bool  want_record      = false;  // set by long-press, consumed in loop

// IMU calibration prompt
static volatile bool  want_calibration = false;  // set on first WiFi up / button A
static volatile bool  calibrating      = false;

// Battery monitoring (AXP192 via M5GFX's I2C bus)
#define AXP192_ADDR   0x34
#define AXP192_I2C_PORT 1
static int  battery_pct = -1;
static bool battery_charging = false;
static uint32_t last_battery_read = 0;
#define BATTERY_READ_INTERVAL_MS 10000

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void draw_main_screen(void);
static void draw_convo_screen(void);
static void draw_settings_screen(void);
static void on_wifi_event(craw_wifi_event_t event, void *ctx);
static void on_mqtt_conn(bool connected, void *ctx);
static void register_forth_words(void);
static int  compute_record_secs(size_t *out_bytes);

// ---------------------------------------------------------------------------
// Hostname / MAC / topic derivation
// ---------------------------------------------------------------------------
static void derive_hostname(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_suffix, sizeof(mac_suffix), "%02x%02x", mac[4], mac[5]);
    snprintf(hostname, sizeof(hostname), "%s-%s", HOSTNAME_PREFIX, mac_suffix);
    ESP_LOGI(TAG, "Hostname: %s, MAC suffix: %s", hostname, mac_suffix);
}

static void derive_mqtt_topic(void) {
    // AXR Worlds Engine telemetry topic. DEVICEID = MAC suffix.
    snprintf(mqtt_topic, sizeof(mqtt_topic),
             "iotj/axrawe/updates/%s", mac_suffix);
}

// ---------------------------------------------------------------------------
// Robot face — works on either M5GFX or M5Canvas (both derive LovyanGFX)
// ~28 wide x 22 tall, anchored at top-left (x,y).
// ---------------------------------------------------------------------------
static void draw_robot_face(lgfx::LovyanGFX &g, int x, int y, uint16_t color,
                            bool blink, int look_dx, bool mouth_open) {
    // Antenna
    g.drawLine(x + 14, y - 4, x + 14, y, color);
    g.fillCircle(x + 14, y - 5, 2, Synth::YELLOW);

    // Head
    g.fillRoundRect(x, y, 28, 22, 4, Synth::DIM_GRAY);
    g.drawRoundRect(x, y, 28, 22, 4, color);

    // Eyes (cyan), shift by look_dx; blink = thin white line
    int eye_y = y + 7;
    int lx = x + 8 + look_dx;
    int rx = x + 20 + look_dx;
    if (blink) {
        g.drawLine(lx - 2, eye_y, lx + 2, eye_y, Synth::WHITE);
        g.drawLine(rx - 2, eye_y, rx + 2, eye_y, Synth::WHITE);
    } else {
        g.fillCircle(lx, eye_y, 2, Synth::CYAN);
        g.fillCircle(rx, eye_y, 2, Synth::CYAN);
    }

    // Mouth — speaker grille; "open" = taller bar (talking)
    int mw = 14, mh = mouth_open ? 6 : 2;
    int mx = x + 7, my = y + 14;
    g.fillRect(mx, my, mw, mh, color);
}

// ---------------------------------------------------------------------------
// Robot animation tick — only fidgets when WiFi is connected.
// ---------------------------------------------------------------------------
static void robot_anim_tick(void) {
    uint32_t now = millis();
    if (!craw_wifi_is_connected()) {
        robot_blink = false;
        robot_look_dx = 0;
        return;
    }

    // Blink scheduling
    if (robot_blink) {
        if (now >= robot_blink_until) robot_blink = false;
    } else if (now >= robot_next_blink) {
        robot_blink = true;
        robot_blink_until = now + 140;
        robot_next_blink  = now + 2000 + (esp_random() % 3000);  // every 2-5 s
    }

    // Occasional "look around"
    if (robot_look_dx != 0) {
        if (now >= robot_look_until) robot_look_dx = 0;
    } else if (now >= robot_next_look) {
        robot_look_dx = (esp_random() % 2) ? 2 : -2;
        robot_look_until = now + 600;
        robot_next_look  = now + 3000 + (esp_random() % 4000);   // every 3-7 s
    }
}

// ---------------------------------------------------------------------------
// Battery: AXP192 I2C read (via M5GFX's lgfx I2C, port 1)
// ---------------------------------------------------------------------------
static void axp192_init_i2c(void) {
    auto val = lgfx::i2c::readRegister8(AXP192_I2C_PORT, AXP192_ADDR, 0x82, 400000);
    if (val.has_value()) {
        uint8_t adc_en = val.value() | 0x80;
        lgfx::i2c::writeRegister8(AXP192_I2C_PORT, AXP192_ADDR, 0x82, adc_en, 0xFF, 400000);
    }
}

static void battery_read(void) {
    uint8_t buf[4] = {0};
    auto result = lgfx::i2c::readRegister(AXP192_I2C_PORT, AXP192_ADDR, 0x78, buf, 4, 400000);
    if (!result.has_value()) { battery_pct = -1; return; }

    uint16_t voltage_raw = ((uint16_t)buf[0] << 4) | (buf[1] & 0x0F);
    uint16_t current_raw = ((uint16_t)buf[2] << 5) | (buf[3] & 0x1F);

    int pct = 0;
    if (voltage_raw > 3150) {
        pct = (int)((voltage_raw - 3075) * 0.16f);
    } else if (voltage_raw > 2690) {
        pct = (int)((voltage_raw - 2690) * 0.027f);
    }
    if (current_raw > 16) pct -= 16;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    battery_pct = pct;

    auto status = lgfx::i2c::readRegister8(AXP192_I2C_PORT, AXP192_ADDR, 0x00, 400000);
    battery_charging = status.has_value() && (status.value() & 0x04) != 0;
}

static void draw_battery_gauge(int x, int y) {
    int gw = 36, gh = 14, nub = 3;
    display.drawRect(x, y, gw, gh, Synth::WHITE);
    display.fillRect(x + gw, y + 3, nub, gh - 6, Synth::WHITE);

    int bar_w = 5, bar_gap = 1, bar_h = gh - 4, bar_y = y + 2, bar_x = x + 2;
    int level = 0;
    if (battery_pct >= 80) level = 5;
    else if (battery_pct >= 60) level = 4;
    else if (battery_pct >= 40) level = 3;
    else if (battery_pct >= 20) level = 2;
    else if (battery_pct > 0) level = 1;

    uint16_t bar_color = Synth::NEON_GREEN;
    if (battery_pct <= 20) bar_color = Synth::RED;
    else if (battery_pct <= 40) bar_color = Synth::YELLOW;
    if (battery_charging) bar_color = Synth::CYAN;

    for (int i = 0; i < 5; i++) {
        uint16_t c = (i < level) ? bar_color : Synth::DIM_GRAY;
        display.fillRect(bar_x + i * (bar_w + bar_gap), bar_y, bar_w, bar_h, c);
    }

    display.setTextSize(1);
    display.setTextColor(Synth::WHITE);
    display.setCursor(x + gw + nub + 4, y + 3);
    if (battery_charging)      display.printf("%d%%+", battery_pct);
    else if (battery_pct >= 0) display.printf("%d%%", battery_pct);
    else                       display.print("?");
}

// ---------------------------------------------------------------------------
// Drawing: MAIN (ROBOTALK XR) screen
// ---------------------------------------------------------------------------
static void draw_main_screen(void) {
    display.fillScreen(Synth::BG);

    display.setTextSize(2);
    display.setTextColor(Synth::CYAN);
    display.setCursor(4, 2);
    display.print("ROBOTALK XR");

    // Robot face top-right (animated when WiFi up)
    draw_robot_face(display, SCREEN_W - 32, 8, Synth::HOT_PINK,
                    robot_blink, robot_look_dx, false);

    display.drawLine(4, 24, SCREEN_W - 4, 24, Synth::GRID);

    char ip_buf[20] = "N/A";
    bool connected = craw_wifi_is_connected();
    if (connected) craw_wifi_get_ip_str(ip_buf, sizeof(ip_buf));

    display.setTextSize(2);
    display.setTextColor(Synth::WHITE);
    display.setCursor(4, 28);
    display.printf("WiFi:%s", connected ? wifi_ssid : "N/A");

    display.setCursor(4, 46);
    display.printf("IP:%s", ip_buf);

    // MQTT + IMU calibration status
    bool mqtt_conn = craw_mqtt_is_connected();
    display.setCursor(4, 64);
    display.setTextColor(mqtt_conn ? Synth::NEON_GREEN : Synth::RED);
    display.printf("MQTT:%s", mqtt_conn ? "OK" : "X");
    display.setTextColor(craw_imu_mpu6886_is_calibrated() ? Synth::NEON_GREEN
                                                          : Synth::YELLOW);
    display.printf(" IMU:%s", craw_imu_mpu6886_is_calibrated() ? "cal" : "raw");

    display.setCursor(4, 82);
    display.setTextColor(Synth::WHITE);
    display.printf("Heap:%lu", (unsigned long)esp_get_free_heap_size());

    display.setCursor(4, 100);
    display.setTextColor(Synth::MAGENTA);
    display.printf("Prof:%s", craw_nvs_active_profile());

    draw_battery_gauge(160, 100);

    display.setTextColor(Synth::TEXT_DIM);
    display.setTextSize(1);
    display.setCursor(4, 122);
    display.print("[A]=Convo  [B]=Settings");
}

// ---------------------------------------------------------------------------
// Drawing: CONVO screen (mic + IMU panel) — drawn to off-screen canvas
// ---------------------------------------------------------------------------
static void draw_convo_screen(void) {
    auto &gfx = canvas;
    gfx.fillScreen(Synth::BG);

    gfx.setTextSize(2);
    gfx.setTextColor(Synth::CYAN);
    gfx.setCursor(4, 2);
    gfx.print("CONVO");

    // Robot face in header — mouth "talks" while recording
    draw_robot_face(gfx, 120, 4, recording ? Synth::NEON_GREEN : Synth::DIM_CYAN,
                    false, 0, recording && ((millis() / 120) % 2 == 0));

    gfx.drawLine(4, 24, SCREEN_W - 4, 24, Synth::GRID);

    // --- Mic row ---
    if (recording) {
        gfx.setTextSize(2);
        gfx.setTextColor(Synth::RED);
        gfx.setCursor(4, 30);
        gfx.print("REC");
        int bx = 56, by = 32, bw = SCREEN_W - bx - 8, bh = 12;
        gfx.drawRect(bx, by, bw, bh, Synth::WHITE);
        int fill = (int)(rec_fraction * (bw - 2));
        if (fill > 0) gfx.fillRect(bx + 1, by + 1, fill, bh - 2, Synth::RED);
    } else {
        gfx.setTextSize(2);
        gfx.setTextColor(Synth::TEXT_DIM);
        gfx.setCursor(4, 30);
        gfx.print("idle");
        gfx.setTextSize(1);
        gfx.setTextColor(Synth::WHITE);
        gfx.setCursor(56, 34);
        if (last_capture_secs > 0.0f) {
            gfx.printf("last %.1fs pk%d", last_capture_secs, last_capture_peak);
        } else {
            gfx.print("hold [B] to record");
        }
    }

    gfx.drawLine(4, 48, SCREEN_W - 4, 48, Synth::GRID);

    // --- IMU readout ---
    craw_imu_mpu6886_snapshot_t s;
    craw_imu_mpu6886_snapshot(&s);
    const float R2D = 57.2957795f;

    gfx.setTextSize(2);
    gfx.setTextColor(Synth::WHITE);
    gfx.setCursor(4, 54);
    gfx.printf("R%+4.0f P%+4.0f", s.roll_rad * R2D, s.pitch_rad * R2D);
    gfx.setCursor(4, 74);
    gfx.printf("Y%+4.0f", s.yaw_rad * R2D);

    gfx.setTextSize(1);
    gfx.setTextColor(Synth::TEXT_DIM);
    gfx.setCursor(118, 78);
    gfx.printf("%.2f %.2f %.2f", s.accel_x, s.accel_y, s.accel_z);

    // Calibration status line
    gfx.setTextSize(1);
    if (calibrating) {
        gfx.setTextColor(Synth::YELLOW);
        gfx.setCursor(4, 94);
        gfx.print("CALIBRATING - hold flat & still...");
    } else {
        gfx.setTextColor(craw_imu_mpu6886_is_calibrated() ? Synth::NEON_GREEN
                                                          : Synth::YELLOW);
        gfx.setCursor(4, 94);
        gfx.printf("IMU: %s", craw_imu_mpu6886_is_calibrated()
                                  ? "calibrated" : "uncalibrated");
    }

    // Live-publish indicator
    gfx.setTextColor(Synth::DIM_CYAN);
    gfx.setCursor(4, 106);
    bool pub = craw_mqtt_is_connected() && craw_imu_mpu6886_is_calibrated();
    gfx.printf("MQTT IMU: %s", pub ? "publishing" : "off");

    // Footer
    gfx.setTextColor(Synth::TEXT_DIM);
    gfx.setCursor(4, 122);
    gfx.print("[A]=Cal hold[B]=Rec [B]=Back");

    canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Drawing: Settings Screen (layout unchanged from Crawdad)
// ---------------------------------------------------------------------------
static void draw_settings_screen(void) {
    display.fillScreen(Synth::BG);

    display.setTextSize(2);
    display.setTextColor(Synth::CYAN);
    display.setCursor(4, 2);
    display.print("SETTINGS");

    display.drawLine(4, 20, SCREEN_W - 4, 20, Synth::GRID);

    const char *items[] = { "Sound", "Bright", "Prof", "Clear", "Back" };
    char values[5][16];
    snprintf(values[0], sizeof(values[0]), "%s", sound_enabled ? "ON" : "OFF");
    snprintf(values[1], sizeof(values[1]), "%d%%", brightness_pct);
    const char *aprof = craw_nvs_active_profile();
    snprintf(values[2], sizeof(values[2]), "%.7s", aprof ? aprof : "?");
    snprintf(values[3], sizeof(values[3]), "[B]");
    snprintf(values[4], sizeof(values[4]), "<-");

    display.setTextSize(2);
    for (int i = 0; i < SETTINGS_ITEMS; i++) {
        int sy = 24 + i * 18;
        bool selected = (i == settings_cursor);
        display.setCursor(4, sy);
        display.setTextColor(selected ? Synth::NEON_GREEN : Synth::DIM_GRAY);
        display.print(selected ? ">" : " ");
        display.setCursor(16, sy);
        display.setTextColor(selected ? Synth::WHITE : Synth::TEXT_DIM);
        display.printf("%s:%s", items[i], values[i]);
    }

    display.setTextSize(1);
    display.setTextColor(Synth::RED);
    display.setCursor(4, 104);
    display.print("Hold[B] 3s=WiFi clr");

    display.setTextColor(Synth::TEXT_DIM);
    display.setCursor(4, 122);
    display.print("[A]=Next  [B]=Toggle");
}

// ---------------------------------------------------------------------------
// WiFi / MQTT callbacks
// ---------------------------------------------------------------------------
static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    switch (event) {
        case CRAW_WIFI_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected");
            craw_http_start(nullptr, nullptr, nullptr);
            craw_mqtt_start();
            // First time WiFi comes up uncalibrated, request a flat calibration.
            if (!craw_imu_mpu6886_is_calibrated()) want_calibration = true;
            dirty_screen = true;
            break;
        case CRAW_WIFI_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WiFi disconnected");
            dirty_screen = true;
            break;
        case CRAW_WIFI_EVENT_CONNECT_FAILED:
            ESP_LOGW(TAG, "WiFi connect failed");
            dirty_screen = true;
            break;
    }
}

static void on_mqtt_conn(bool connected, void *ctx) {
    ESP_LOGI(TAG, "MQTT %s", connected ? "connected" : "disconnected");
    dirty_screen = true;
}

// ---------------------------------------------------------------------------
// IMU MQTT publish (called from main loop on a cadence)
// ---------------------------------------------------------------------------
static char imu_json[256];
static void publish_imu(void) {
    craw_imu_mpu6886_snapshot_t s;
    craw_imu_mpu6886_snapshot(&s);
    snprintf(imu_json, sizeof(imu_json),
        "{\"id\":\"%s\",\"ts\":%llu,"
        "\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,"
        "\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,"
        "\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f}",
        mac_suffix, (unsigned long long)s.timestamp_us,
        s.accel_x, s.accel_y, s.accel_z,
        s.gyro_x, s.gyro_y, s.gyro_z,
        s.roll_rad, s.pitch_rad, s.yaw_rad);
    craw_mqtt_publish(mqtt_topic, imu_json, 0, false);
}

// ---------------------------------------------------------------------------
// Mic recording task (spawned per long-press)
// ---------------------------------------------------------------------------
static void rec_progress_cb(float frac, void *ctx) {
    rec_fraction = frac;
    dirty_screen = true;
}

// Decide how many seconds to capture given current free internal DRAM.
static int compute_record_secs(size_t *out_bytes) {
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Use at most half the largest contiguous block; leave headroom for stacks.
    size_t budget = largest / 2;
    int secs = craw_mic_max_record_seconds(budget);
    if (secs > MIC_MAX_RECORD_SECS) secs = MIC_MAX_RECORD_SECS;
    if (secs < 1) secs = 1;
    if (out_bytes) *out_bytes = (size_t)secs * craw_mic_bytes_per_sec();
    return secs;
}

static void record_task(void *param) {
    size_t buf_bytes = 0;
    int secs = compute_record_secs(&buf_bytes);
    size_t max_samples = buf_bytes / sizeof(int16_t);

    int16_t *buf = (int16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "record: alloc %u bytes failed", (unsigned)buf_bytes);
        recording = false;
        dirty_screen = true;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "record: %d s (%u bytes, %u samples)",
             secs, (unsigned)buf_bytes, (unsigned)max_samples);
    craw_speaker_tone(1500, 80);   // start beep

    recording = true;
    rec_fraction = 0;
    dirty_screen = true;

    size_t got = 0;
    craw_mic_record(buf, max_samples, secs, &got, rec_progress_cb, nullptr);

    int peak = craw_mic_peak(buf, got);
    last_capture_secs = (float)got / (float)craw_mic_sample_rate();
    last_capture_peak = peak;
    recording = false;
    dirty_screen = true;

    craw_speaker_tone(900, 80);    // stop beep

    // Publish a lightweight "voice event" so the AXR engine knows a capture
    // happened (audio itself stays on-device — no PSRAM to stream from).
    if (craw_mqtt_is_connected()) {
        char ev[160];
        snprintf(ev, sizeof(ev),
                 "{\"id\":\"%s\",\"evt\":\"voice\",\"dur_s\":%.2f,\"peak\":%d}",
                 mac_suffix, last_capture_secs, peak);
        craw_mqtt_publish(mqtt_topic, ev, 0, false);
    }

    ESP_LOGI(TAG, "record done: %.2f s, peak=%d", last_capture_secs, peak);
    heap_caps_free(buf);
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// IMU calibration (foreground; blocks main loop ~2s with on-screen prompt)
// ---------------------------------------------------------------------------
static void run_calibration(void) {
    calibrating = true;
    current_screen = SCREEN_CONVO;

    // Short countdown so the user can set it flat.
    for (int n = 2; n >= 1; n--) {
        draw_convo_screen();
        craw_serial_printf("[IMU] calibrating in %d... keep flat & still\r\n", n);
        vTaskDelay(pdMS_TO_TICKS(700));
    }

    craw_imu_mpu6886_calibrate(0);   // default ~2 s averaging
    calibrating = false;
    craw_speaker_tone(1800, 60);
    dirty_screen = true;
    craw_serial_print("[IMU] calibration complete.\r\n");
}

// ---------------------------------------------------------------------------
// Button handling
// ---------------------------------------------------------------------------
static void button_init(void) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pin_bit_mask = (1ULL << BUTTON_A_GPIO) | (1ULL << BUTTON_B_GPIO);
    gpio_config(&io_conf);
}

static void handle_buttons(void) {
    bool btn_a = gpio_get_level(BUTTON_A_GPIO);  // active low
    bool btn_b = gpio_get_level(BUTTON_B_GPIO);

    // ---- Button A: rising edge (released) ----
    if (btn_a && !btn_a_last) {
        switch (current_screen) {
            case SCREEN_WIFI:
                current_screen = SCREEN_CONVO;
                dirty_screen = true;
                break;
            case SCREEN_CONVO:
                if (!recording && !calibrating) want_calibration = true;  // re-cal
                break;
            case SCREEN_SETTINGS:
                settings_cursor = (settings_cursor + 1) % SETTINGS_ITEMS;
                dirty_screen = true;
                break;
        }
    }

    // ---- Button B: press edge ----
    if (!btn_b && btn_b_last) {
        btn_b_press_time = millis();
        btn_b_long_fired = false;
    }

    // ---- Button B: held (fire long-press once) ----
    if (!btn_b && !btn_b_last && !btn_b_long_fired) {
        uint32_t held = millis() - btn_b_press_time;
        if (current_screen == SCREEN_SETTINGS && held >= LONGPRESS_WIFI_MS) {
            btn_b_long_fired = true;
            ESP_LOGW(TAG, "Long press B: clearing WiFi and rebooting");
            craw_serial_printf("Long press B: clearing WiFi credentials and rebooting...\r\n");
            craw_nvs_clear_wifi_creds(wifi_ssid, wifi_pass);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else if (current_screen == SCREEN_CONVO && held >= LONGPRESS_RECORD_MS) {
            btn_b_long_fired = true;
            if (!recording && !calibrating) want_record = true;
        }
    }

    // ---- Button B: rising edge (released) — short press ----
    if (btn_b && !btn_b_last && !btn_b_long_fired) {
        if (current_screen == SCREEN_SETTINGS) {
            switch (settings_cursor) {
                case 0: // Sound
                    sound_enabled = !sound_enabled;
                    craw_speaker_set_sound_enabled(sound_enabled);
                    craw_nvs_save_sound_pref(sound_enabled);
                    if (sound_enabled) craw_speaker_chime_working();
                    break;
                case 1: // Brightness
                    brightness_pct += 20;
                    if (brightness_pct > 100) brightness_pct = 20;
                    display.setBrightness((brightness_pct * 255) / 100);
                    break;
                case 2: // Profile — cycle + reconnect
                    {
                        int pcount = craw_nvs_profile_count();
                        if (pcount > 0) {
                            const char *cur = craw_nvs_active_profile();
                            int cur_idx = craw_nvs_profile_find(cur);
                            int next_idx = (cur_idx + 1) % pcount;
                            int dummy_count = 0;
                            auto profiles = craw_nvs_profile_list(&dummy_count);
                            const char *next_name = profiles[next_idx];
                            craw_nvs_set_active_profile(next_name);
                            craw_nvs_load_profile_creds(next_name, wifi_ssid, wifi_pass);
                            craw_serial_printf("[PROF] Switched to '%s'\r\n", next_name);
                            if (wifi_ssid[0]) craw_wifi_connect(wifi_ssid, wifi_pass);
                        }
                    }
                    break;
                case 3: // Clear — clears IMU calibration (forces re-cal)
                    craw_imu_mpu6886_clear_cal();
                    craw_serial_print("[IMU] calibration cleared.\r\n");
                    break;
                case 4: // Back
                    current_screen = SCREEN_WIFI;
                    settings_cursor = 0;
                    break;
            }
            dirty_screen = true;
        } else if (current_screen == SCREEN_CONVO) {
            current_screen = SCREEN_WIFI;     // short B = back
            dirty_screen = true;
        } else { // WIFI -> Settings
            current_screen = SCREEN_SETTINGS;
            dirty_screen = true;
        }
    }

    btn_a_last = btn_a;
    btn_b_last = btn_b;
}

// ---------------------------------------------------------------------------
// Forth REPL task
// ---------------------------------------------------------------------------
static void forth_repl_task(void *param) {
    craw_serial_print("\r\nForth REPL ready. Type 'words' for list.\r\n");
    forth_repl(craw_serial_getchar, craw_serial_putchar);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Forth word implementations
// ---------------------------------------------------------------------------
static void w_wifi_ssid(void) {
    craw_serial_printf("Active profile: '%s'\r\n", craw_nvs_active_profile());
    craw_serial_print("SSID: ");
    craw_serial_read_line(wifi_ssid, sizeof(wifi_ssid), true, false);
    craw_serial_printf("SSID set to: '%s'\r\n", wifi_ssid);
}

static void w_wifi_pass(void) {
    craw_serial_printf("Active profile: '%s'\r\n", craw_nvs_active_profile());
    craw_serial_print("Password (Enter for open): ");
    craw_serial_read_line(wifi_pass, sizeof(wifi_pass), false, true);
    craw_serial_print(wifi_pass[0] ? "Password set.\r\n" : "No password (open network)\r\n");
}

static void w_wifi_connect(void) {
    if (wifi_ssid[0] == '\0') { craw_serial_print("No SSID set. Use wifi-ssid first.\r\n"); return; }
    craw_nvs_save_wifi_creds(wifi_ssid, wifi_pass);
    craw_serial_printf("Saved to profile '%s'. Connecting to '%s'...\r\n",
                       craw_nvs_active_profile(), wifi_ssid);
    craw_wifi_connect(wifi_ssid, wifi_pass);
}

static void w_wifi_status(void) {
    craw_serial_printf("Profile: '%s' (%d of %d)\r\n",
        craw_nvs_active_profile(),
        craw_nvs_profile_find(craw_nvs_active_profile()) + 1,
        craw_nvs_profile_count());
    char ip_buf[20] = "N/A";
    if (craw_wifi_is_connected()) craw_wifi_get_ip_str(ip_buf, sizeof(ip_buf));
    craw_serial_printf("SSID:       '%s'\r\n", wifi_ssid);
    craw_serial_printf("Connected:  %s\r\n", craw_wifi_is_connected() ? "yes" : "no");
    craw_serial_printf("IP:         %s\r\n", ip_buf);
}

static void w_wifi_clear(void) {
    craw_serial_printf("Clearing credentials for profile '%s'...\r\n",
                       craw_nvs_active_profile());
    craw_nvs_clear_wifi_creds(wifi_ssid, wifi_pass);
    craw_wifi_disconnect();
    craw_serial_print("WiFi credentials cleared.\r\n");
    dirty_screen = true;
}

static void w_wifi_profiles(void) {
    int cnt = 0;
    auto list = craw_nvs_profile_list(&cnt);
    craw_serial_printf("Profiles (%d):\r\n", cnt);
    for (int i = 0; i < cnt; i++) {
        bool active = (strcmp(list[i], craw_nvs_active_profile()) == 0);
        craw_serial_printf("  %d. %s%s\r\n", i + 1, list[i], active ? " [active]" : "");
    }
    craw_serial_print("Enter name or number to switch (Enter to cancel): ");
    char input[32] = {0};
    craw_serial_read_line(input, sizeof(input), true, true);
    if (input[0] == '\0') { craw_serial_print("Cancelled.\r\n"); return; }
    int n = atoi(input);
    if (n >= 1 && n <= cnt) {
        craw_nvs_set_active_profile(list[n - 1]);
        craw_nvs_load_wifi_creds(wifi_ssid, wifi_pass);
        craw_serial_printf("Switched to profile '%s'\r\n", craw_nvs_active_profile());
        return;
    }
    if (craw_nvs_profile_find(input) >= 0) {
        craw_nvs_set_active_profile(input);
        craw_nvs_load_wifi_creds(wifi_ssid, wifi_pass);
        craw_serial_printf("Switched to profile '%s'\r\n", input);
        return;
    }
    if (craw_nvs_profile_count() >= CRAW_PROFILE_MAX_COUNT) {
        craw_serial_printf("Maximum profiles (%d) reached.\r\n", CRAW_PROFILE_MAX_COUNT);
        return;
    }
    if (!craw_nvs_profile_name_valid(input)) { craw_serial_print("Invalid profile name.\r\n"); return; }
    craw_nvs_profile_add(input);
    craw_nvs_profile_list_save();
    craw_nvs_set_active_profile(input);
    craw_serial_print("Created new profile. Use wifi-ssid / wifi-pass / wifi-connect.\r\n");
}

static void w_mqtt_broker(void) {
    craw_serial_printf("Current: %s\r\n", mqtt_broker);
    craw_serial_print("New broker URI (Enter to keep): ");
    char buf[128] = {0};
    craw_serial_read_line(buf, sizeof(buf), true, true);
    if (buf[0] != '\0') {
        strncpy(mqtt_broker, buf, sizeof(mqtt_broker) - 1);
        craw_nvs_save_mqtt_broker(mqtt_broker);
        craw_mqtt_set_broker(mqtt_broker);
        craw_serial_printf("Broker set to: %s\r\n", mqtt_broker);
        if (craw_wifi_is_connected()) {
            craw_mqtt_stop();
            craw_mqtt_init(mqtt_broker, "", nullptr, on_mqtt_conn, nullptr);
            craw_mqtt_start();
        }
    } else {
        craw_serial_print("Unchanged.\r\n");
    }
}

static void w_mqtt_status(void) {
    craw_serial_printf("Broker:    %s\r\n", mqtt_broker);
    craw_serial_printf("Topic:     %s\r\n", mqtt_topic[0] ? mqtt_topic : "(not set)");
    craw_serial_printf("Connected: %s\r\n", craw_mqtt_is_connected() ? "yes" : "no");
}

static void w_mqtt_topic(void) {
    if (mqtt_topic[0] == '\0') derive_mqtt_topic();
    craw_serial_printf("%s\r\n", mqtt_topic);
}

static void w_sound_on(void) {
    sound_enabled = true;
    craw_speaker_set_sound_enabled(true);
    craw_nvs_save_sound_pref(true);
    craw_serial_print("Sound enabled.\r\n");
    craw_speaker_tone(1200, 60);
}

static void w_sound_off(void) {
    sound_enabled = false;
    craw_speaker_set_sound_enabled(false);
    craw_nvs_save_sound_pref(false);
    craw_serial_print("Sound disabled.\r\n");
}

static void w_brightness(void) {
    int n = (int)forth_pop();
    if (n < 0) n = 0;
    if (n > 100) n = 100;
    brightness_pct = n;
    display.setBrightness((brightness_pct * 255) / 100);
    craw_serial_printf("Brightness: %d%%\r\n", brightness_pct);
}

// ---- IMU / mic Forth words ----
static void w_imu_cal(void) {
    craw_serial_print("Place the device flat & still. Calibrating...\r\n");
    want_calibration = true;
}

static void w_imu_status(void) {
    craw_imu_mpu6886_stats_t st;
    craw_imu_mpu6886_stats(&st);
    craw_imu_mpu6886_snapshot_t s;
    craw_imu_mpu6886_snapshot(&s);
    const float R2D = 57.2957795f;
    craw_serial_printf("init=%d run=%d cal=%d samples=%llu hz=%d\r\n",
        st.initialized, st.running, st.calibrated,
        (unsigned long long)st.samples, st.sample_hz);
    craw_serial_printf("roll=%.1f pitch=%.1f yaw=%.1f deg\r\n",
        s.roll_rad * R2D, s.pitch_rad * R2D, s.yaw_rad * R2D);
    craw_serial_printf("accel=(%.2f,%.2f,%.2f) m/s2  gyro=(%.3f,%.3f,%.3f) rad/s\r\n",
        s.accel_x, s.accel_y, s.accel_z, s.gyro_x, s.gyro_y, s.gyro_z);
    craw_serial_printf("gyro bias=(%.4f,%.4f,%.4f)\r\n",
        st.gyro_bias_x, st.gyro_bias_y, st.gyro_bias_z);
}

static void w_imu_zero(void) {
    craw_imu_mpu6886_zero_yaw();
    craw_serial_print("Yaw datum reset.\r\n");
}

static void w_mic_secs(void) {
    size_t bytes = 0;
    int secs = compute_record_secs(&bytes);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    craw_serial_printf("Sample rate:   %d Hz (%u B/s)\r\n",
        craw_mic_sample_rate(), (unsigned)craw_mic_bytes_per_sec());
    craw_serial_printf("Largest block: %u bytes\r\n", (unsigned)largest);
    craw_serial_printf("Max capture:   %d s (uses %u bytes, capped at %d s)\r\n",
        secs, (unsigned)bytes, MIC_MAX_RECORD_SECS);
}

static void w_mic_test(void) {
    if (recording || calibrating) { craw_serial_print("Busy.\r\n"); return; }
    craw_serial_print("Recording...\r\n");
    want_record = true;
}

static void register_forth_words(void) {
    forth_register_word("wifi-ssid",     w_wifi_ssid);
    forth_register_word("wifi-pass",     w_wifi_pass);
    forth_register_word("wifi-connect",  w_wifi_connect);
    forth_register_word("wifi-status",   w_wifi_status);
    forth_register_word("wifi-clear",    w_wifi_clear);
    forth_register_word("wifi-profiles", w_wifi_profiles);
    forth_register_word("mqtt-broker",   w_mqtt_broker);
    forth_register_word("mqtt-status",   w_mqtt_status);
    forth_register_word("mqtt-topic",    w_mqtt_topic);
    forth_register_word("sound-on",      w_sound_on);
    forth_register_word("sound-off",     w_sound_off);
    forth_register_word("brightness",    w_brightness);
    forth_register_word("imu-cal",       w_imu_cal);
    forth_register_word("imu-status",    w_imu_status);
    forth_register_word("imu-zero",      w_imu_zero);
    forth_register_word("mic-secs",      w_mic_secs);
    forth_register_word("mic-test",      w_mic_test);
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
extern "C" void app_main(void)
{
    craw_serial_init();
    vTaskDelay(pdMS_TO_TICKS(300));

    craw_serial_print("\r\n\r\n");
    craw_serial_print("============================================\r\n");
    craw_serial_printf("  ROBOTALK XR v%s\r\n", VERSION_STRING);
    craw_serial_printf("  Forth %s | Build: %s %s\r\n",
        ESPIDFORTH_VERSION_STRING, ESPIDFORTH_BUILD_DATE, ESPIDFORTH_BUILD_TIME);
    craw_serial_print("  Voice + IMU XR node (AXR Worlds Engine)\r\n");
    craw_serial_print("============================================\r\n");

    // Display init (M5GFX auto-detects M5StickC Plus, owns the AXP192 I2C bus)
    display.init();
    display.setRotation(1);  // landscape 240x135, USB on right
    display.fillScreen(Synth::BG);
    display.setTextWrap(false);
    canvas.createSprite(SCREEN_W, SCREEN_H);
    canvas.setTextWrap(false);
    display.setBrightness((brightness_pct * 255) / 100);
    craw_serial_print("Display initialized.\r\n");

    // AXP192 battery monitor (shares M5GFX's I2C bus)
    axp192_init_i2c();
    battery_read();
    craw_serial_printf("Battery: %d%%%s\r\n", battery_pct, battery_charging ? " (charging)" : "");

    // Speaker (GPIO 2)
    craw_speaker_init(SPEAKER_GPIO);

    // Buttons
    button_init();

    // NVS
    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    craw_nvs_profiles_load();

    sound_enabled = craw_nvs_load_sound_pref();
    craw_speaker_set_sound_enabled(sound_enabled);
    craw_serial_printf("Sound: %s\r\n", sound_enabled ? "on" : "off");

    craw_nvs_load_mqtt_broker(mqtt_broker, sizeof(mqtt_broker));
    craw_serial_printf("MQTT broker: %s\r\n", mqtt_broker);

    derive_hostname();
    derive_mqtt_topic();
    craw_serial_printf("MQTT topic: %s\r\n", mqtt_topic);

    // IMU (MUST be after display.init — shares the M5GFX I2C bus)
    {
        craw_imu_mpu6886_config_t icfg = {};   // all defaults
        esp_err_t e = craw_imu_mpu6886_init(&icfg);
        if (e == ESP_OK) {
            craw_imu_mpu6886_start();
            craw_serial_print("IMU MPU6886 started.\r\n");
        } else {
            craw_serial_printf("IMU init failed (0x%x) - check it's an MPU6886.\r\n", e);
        }
    }

    // Mic (I2S PDM-RX; reserves I2S0)
    {
        craw_mic_config_t mcfg = {};  // defaults: clk=0 din=34 rate=16000
        if (craw_mic_init(&mcfg) == ESP_OK) {
            craw_serial_printf("Mic ready: up to %d s capture @ %d Hz\r\n",
                MIC_MAX_RECORD_SECS, craw_mic_sample_rate());
        } else {
            craw_serial_print("Mic init failed.\r\n");
        }
    }

    // WiFi
    craw_wifi_init(hostname, on_wifi_event, nullptr);

    // MQTT — publisher-only (no subscribe); we push IMU + voice events.
    craw_mqtt_init(mqtt_broker, "", nullptr, on_mqtt_conn, nullptr);

    // Forth
    forth_init(FORTH_HEAP_SIZE);
    register_forth_words();
    craw_serial_print("Forth engine initialized.\r\n");
    craw_serial_printf("Free heap: %lu bytes\r\n\r\n",
        (unsigned long)esp_get_free_heap_size());

    craw_serial_print("Forth commands:\r\n");
    craw_serial_print("  N brightness  -- set brightness (0-100)\r\n");
    craw_serial_print("  wifi-ssid / wifi-pass / wifi-connect / wifi-status\r\n");
    craw_serial_print("  wifi-clear / wifi-profiles\r\n");
    craw_serial_print("  mqtt-broker / mqtt-status / mqtt-topic\r\n");
    craw_serial_print("  sound-on / sound-off\r\n");
    craw_serial_print("  imu-cal / imu-status / imu-zero\r\n");
    craw_serial_print("  mic-secs / mic-test\r\n\r\n");

    xTaskCreate(forth_repl_task, "forth_repl", 8192, NULL, 3, NULL);

    current_screen = SCREEN_WIFI;
    draw_main_screen();
    dirty_screen = false;

    // Auto-connect WiFi from NVS
    if (craw_nvs_load_wifi_creds(wifi_ssid, wifi_pass)) {
        craw_serial_printf("Stored WiFi: '%s' -- connecting...\r\n", wifi_ssid);
        craw_wifi_connect(wifi_ssid, wifi_pass);
    } else {
        craw_serial_print("No stored WiFi. Configure via Forth REPL.\r\n");
    }

    // Main loop
    uint32_t last_anim_tick   = millis();
    uint32_t last_imu_publish = millis();

    while (1) {
        uint32_t now = millis();

        handle_buttons();

        // Deferred record trigger (own task so UI stays live)
        if (want_record && !recording && !calibrating) {
            want_record = false;
            xTaskCreate(record_task, "rec", 4096, NULL, 5, NULL);
        }

        // Deferred IMU calibration (foreground, blocks briefly with a prompt)
        if (want_calibration && !recording && !calibrating) {
            want_calibration = false;
            run_calibration();
        }

        // Robot fidget + animation (every 150 ms)
        if (now - last_anim_tick >= 150) {
            last_anim_tick = now;
            robot_anim_tick();
            if (current_screen == SCREEN_WIFI && craw_wifi_is_connected()) {
                display.fillRect(SCREEN_W - 34, 0, 34, 24, Synth::BG);
                draw_robot_face(display, SCREEN_W - 32, 8, Synth::HOT_PINK,
                                robot_blink, robot_look_dx, false);
            } else if (current_screen == SCREEN_CONVO) {
                dirty_screen = true;   // live IMU + mic readout
            }
        }

        // IMU MQTT publish (~10 Hz) once calibrated + connected
        if (craw_mqtt_is_connected() && craw_imu_mpu6886_is_calibrated() &&
            !recording && (now - last_imu_publish >= IMU_PUBLISH_INTERVAL_MS)) {
            last_imu_publish = now;
            publish_imu();
        }

        // Battery (every 10 s)
        if (now - last_battery_read >= BATTERY_READ_INTERVAL_MS) {
            battery_read();
            last_battery_read = now;
            if (current_screen == SCREEN_WIFI) dirty_screen = true;
        }

        // Screen drawing
        if (dirty_screen) {
            switch (current_screen) {
                case SCREEN_WIFI:     draw_main_screen();     break;
                case SCREEN_CONVO:    draw_convo_screen();    break;
                case SCREEN_SETTINGS: draw_settings_screen(); break;
            }
            dirty_screen = false;
        }

        craw_speaker_update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
