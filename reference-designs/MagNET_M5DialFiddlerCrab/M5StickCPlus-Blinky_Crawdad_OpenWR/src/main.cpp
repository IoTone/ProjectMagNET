// M5StickCPlus-Blinky_Crawdad_OpenWR — Multi-Session Claude Code Monitor
// Synthwave dashboard on M5StickC Plus (240x135 landscape ST7789, ESP32 classic).
// Monitors up to 8 concurrent Claude CLI sessions via MQTT wildcard topics.
// Uses shared craw_* components for WiFi, NVS, MQTT, HTTP, speaker, serial.
// Includes ESPIDFORTH REPL for configuration.

#include <M5GFX.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

#include "craw_serial.h"
#include "craw_speaker.h"
#include "craw_wifi.h"
#include "craw_nvs.h"
#include "craw_mqtt.h"
#include "craw_http.h"
#include "forth_core.h"
#include "forth_version.h"

#include <cstdio>
#include <cstring>
#include <cmath>

static const char *TAG = "crawdad";

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
#define FORTH_HEAP_SIZE      (32 * 1024)
#define HOSTNAME_PREFIX      "Crawdad"
#define VERSION_STRING       "0.1.0"

#define BUTTON_A_GPIO        GPIO_NUM_37
#define BUTTON_B_GPIO        GPIO_NUM_39
#define SPEAKER_GPIO         2
#define LED_GPIO             GPIO_NUM_10

#define SCREEN_W             240
#define SCREEN_H             135

#define MAX_SESSIONS         8
#define SESSION_ID_LEN       39  // UUIDs are 36 chars, allow a few extra
#define STALE_TIMEOUT_MS     (5  * 60 * 1000)   // 5 min -> dimmed
#define REMOVE_TIMEOUT_MS    (30 * 60 * 1000)   // 30 min -> removed
#define STALENESS_CHECK_MS   10000               // check every 10s

// States (match craw_mqtt_msg_t.state)
#define STATE_IDLE           0
#define STATE_WORKING        2
#define STATE_NEED_INPUT     3
#define STATE_FINISHED       5
#define STATE_ERROR          7

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

// Session color palette (round-robin assignment)
static const uint16_t SESSION_COLORS[] = {
    Synth::CYAN, Synth::MAGENTA, Synth::HOT_PINK, Synth::NEON_GREEN, Synth::YELLOW
};
static const int NUM_SESSION_COLORS = 5;

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
static M5GFX display;
static M5Canvas canvas(&display);  // off-screen sprite for flicker-free drawing

// ---------------------------------------------------------------------------
// Session data
// ---------------------------------------------------------------------------
typedef struct {
    char     id[SESSION_ID_LEN + 1];
    char     model[16];
    char     host[16];
    int      state;
    int      session_pct;
    uint32_t last_seen;
    uint32_t work_start;
    uint32_t work_accumulated;
    bool     timer_running;
    bool     active;
    uint8_t  color_index;
} session_t;

static session_t sessions[MAX_SESSIONS];
static int session_count = 0;
static uint8_t next_color_index = 0;

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
enum screen_t { SCREEN_WIFI = 0, SCREEN_SESSIONS, SCREEN_SETTINGS };
static screen_t current_screen = SCREEN_WIFI;

static char hostname[32]    = {0};
static char mac_suffix[8]   = {0};
static char mqtt_topic[128] = {0};
static char mqtt_broker[128] = {0};
static char wifi_ssid[33]   = {0};
static char wifi_pass[65]   = {0};

static int  brightness_pct  = 80;
static bool sound_enabled   = true;

// Dirty flags for partial redraw
static volatile bool dirty_wifi     = true;
static volatile bool dirty_sessions = true;
static volatile bool dirty_settings = true;
static volatile bool dirty_screen   = true;   // full screen repaint

// Settings screen
static int  settings_cursor = 0;
#define SETTINGS_ITEMS 5  // Sound, Brightness, Profile, Clear Sessions, Back

// Sessions screen scroll
static int session_scroll_offset = 0;
#define SESSIONS_VISIBLE_ROWS 5  // size 2 rows (~20px each) in 100px content area

// Button state
static bool btn_a_last = true;   // active low, true = released
static bool btn_b_last = true;
static uint32_t btn_b_press_time = 0;
static bool btn_b_long_fired = false;

// Crawdad animation
static uint32_t crawdad_anim_tick = 0;
static int crawdad_leg_phase = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void draw_wifi_screen(void);
static void draw_sessions_screen(void);
static void draw_settings_screen(void);
static void on_wifi_event(craw_wifi_event_t event, void *ctx);
static void on_mqtt_message(const craw_mqtt_msg_t *msg, void *ctx);
static void on_mqtt_conn(bool connected, void *ctx);
static void on_http_notify(const craw_mqtt_msg_t *msg, void *ctx);
static const char *on_http_status(void *ctx);
static void register_forth_words(void);

// ---------------------------------------------------------------------------
// Hostname / MAC derivation
// ---------------------------------------------------------------------------
static void derive_hostname(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_suffix, sizeof(mac_suffix), "%02x%02x", mac[4], mac[5]);
    snprintf(hostname, sizeof(hostname), "%s-%s", HOSTNAME_PREFIX, mac_suffix);
    ESP_LOGI(TAG, "Hostname: %s, MAC suffix: %s", hostname, mac_suffix);
}

static void derive_mqtt_topic(void) {
    snprintf(mqtt_topic, sizeof(mqtt_topic),
             "iotj/cl/openwr/updates/%s", mac_suffix);
}

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------
static session_t *find_session(const char *id) {
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active && strcmp(sessions[i].id, id) == 0) {
            return &sessions[i];
        }
    }
    return nullptr;
}

static session_t *create_session(const char *id) {
    if (session_count >= MAX_SESSIONS) {
        // Evict oldest stale session, or oldest session
        int evict = -1;
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (sessions[i].last_seen < oldest) {
                oldest = sessions[i].last_seen;
                evict = i;
            }
        }
        if (evict >= 0) {
            ESP_LOGI(TAG, "Evicting session %s for %s", sessions[evict].id, id);
            memset(&sessions[evict], 0, sizeof(session_t));
            session_count--;
            // Compact: shift down
            for (int i = evict; i < MAX_SESSIONS - 1; i++) {
                sessions[i] = sessions[i + 1];
            }
            memset(&sessions[MAX_SESSIONS - 1], 0, sizeof(session_t));
        }
    }

    // Find first inactive slot
    int slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) { slot = i; break; }
    }
    if (slot < 0) return nullptr;

    session_t *s = &sessions[slot];
    memset(s, 0, sizeof(session_t));
    strncpy(s->id, id, SESSION_ID_LEN);
    s->id[SESSION_ID_LEN] = '\0';
    s->active = true;
    s->color_index = next_color_index;
    next_color_index = (next_color_index + 1) % NUM_SESSION_COLORS;
    s->last_seen = millis();
    s->session_pct = -1;
    if (slot >= session_count) session_count = slot + 1;
    return s;
}

static void remove_session(int idx) {
    if (idx < 0 || idx >= MAX_SESSIONS) return;
    sessions[idx].active = false;
    // Compact
    for (int i = idx; i < MAX_SESSIONS - 1; i++) {
        sessions[i] = sessions[i + 1];
    }
    memset(&sessions[MAX_SESSIONS - 1], 0, sizeof(session_t));
    if (session_count > 0) session_count--;
}

static void clear_all_sessions(void) {
    memset(sessions, 0, sizeof(sessions));
    session_count = 0;
    next_color_index = 0;
    dirty_sessions = true;
}

static int count_active_sessions(void) {
    int n = 0;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active) n++;
    }
    return n;
}

static const char *state_letter(int st) {
    switch (st) {
        case STATE_WORKING:    return "W";
        case STATE_FINISHED:   return "F";
        case STATE_IDLE:       return "I";
        case STATE_NEED_INPUT: return "N";
        case STATE_ERROR:      return "E";
        default:               return "?";
    }
}

static uint16_t state_color(int st) {
    switch (st) {
        case STATE_WORKING:    return Synth::NEON_GREEN;
        case STATE_FINISHED:   return Synth::CYAN;
        case STATE_NEED_INPUT: return Synth::YELLOW;
        case STATE_ERROR:      return Synth::RED;
        case STATE_IDLE:       return Synth::TEXT_DIM;
        default:               return Synth::DIM_GRAY;
    }
}

static bool session_is_stale(const session_t *s) {
    return (millis() - s->last_seen) > STALE_TIMEOUT_MS;
}

// Truncate model name for display: "opus-4-6" -> "opus", "sonnet-4-6" -> "sonn"
static void truncate_model(const char *model, char *out, int maxlen) {
    // Extract base name before first '-' or digit
    if (strncmp(model, "sonnet", 6) == 0) {
        strncpy(out, "sonn", maxlen);
    } else if (strncmp(model, "haiku", 5) == 0) {
        strncpy(out, "haik", maxlen);
    } else if (strncmp(model, "opus", 4) == 0) {
        strncpy(out, "opus", maxlen);
    } else {
        strncpy(out, model, maxlen);
    }
    out[maxlen - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Session timer helpers
// ---------------------------------------------------------------------------
static void session_timer_start(session_t *s) {
    if (!s->timer_running) {
        s->work_start = millis();
        s->timer_running = true;
    }
}

static void session_timer_pause(session_t *s) {
    if (s->timer_running) {
        s->work_accumulated += millis() - s->work_start;
        s->timer_running = false;
    }
}

static uint32_t session_timer_elapsed_ms(const session_t *s) {
    uint32_t total = s->work_accumulated;
    if (s->timer_running) {
        total += millis() - s->work_start;
    }
    return total;
}

static void session_timer_reset(session_t *s) {
    s->work_accumulated = 0;
    s->work_start = millis();
    s->timer_running = false;
}

// ---------------------------------------------------------------------------
// Crawdad mini character drawing (~20x12 pixels)
// ---------------------------------------------------------------------------
static void draw_crawdad_mini(int x, int y, uint16_t color, bool animate) {
    int phase = animate ? crawdad_leg_phase : 0;

    // Body: two small overlapping circles
    display.fillCircle(x, y + 4, 4, color);          // rear body
    display.fillCircle(x + 6, y + 3, 3, color);      // front body

    // Eyes: two dots
    display.fillCircle(x + 10, y + 1, 1, Synth::WHITE);
    display.fillCircle(x + 10, y + 5, 1, Synth::WHITE);

    // Claws: two small arcs (drawn as short lines)
    display.drawLine(x + 9, y,     x + 13, y - 2, color);   // top claw
    display.drawLine(x + 13, y - 2, x + 11, y - 3, color);  // top claw tip
    display.drawLine(x + 9, y + 6, x + 13, y + 8, color);   // bottom claw
    display.drawLine(x + 13, y + 8, x + 11, y + 9, color);  // bottom claw tip

    // Legs: 4 short lines on bottom, with animation phase shift
    int dy = (phase % 2 == 0) ? 0 : 1;
    display.drawLine(x - 2, y + 5 + dy, x - 4, y + 8 + dy, color);
    display.drawLine(x + 0, y + 6 - dy, x - 2, y + 9 - dy, color);
    display.drawLine(x + 2, y + 6 + dy, x + 0, y + 9 + dy, color);
    display.drawLine(x + 4, y + 5 - dy, x + 2, y + 8 - dy, color);
}

// ---------------------------------------------------------------------------
// Drawing: WiFi Status Screen
// ---------------------------------------------------------------------------
static void draw_wifi_screen(void) {
    display.fillScreen(Synth::BG);

    // Header (size 2)
    display.setTextSize(2);
    display.setTextColor(Synth::CYAN);
    display.setCursor(4, 2);
    display.print("CRAWDAD OpenWR");

    // Crawdad mini in top-right corner (animated!)
    draw_crawdad_mini(SCREEN_W - 30, 4, Synth::HOT_PINK, true);

    // Divider line
    display.drawLine(4, 20, SCREEN_W - 4, 20, Synth::GRID);

    // WiFi info
    char ip_buf[20] = "N/A";
    bool connected = craw_wifi_is_connected();
    if (connected) {
        craw_wifi_get_ip_str(ip_buf, sizeof(ip_buf));
    }

    display.setTextSize(2);
    display.setTextColor(Synth::WHITE);
    display.setCursor(4, 24);
    display.printf("WiFi:%s", connected ? wifi_ssid : "N/A");

    display.setCursor(4, 42);
    display.printf("IP:%s", ip_buf);

    // MQTT + Sessions on same row
    bool mqtt_conn = craw_mqtt_is_connected();
    display.setCursor(4, 60);
    display.setTextColor(mqtt_conn ? Synth::NEON_GREEN : Synth::RED);
    display.printf("MQTT:%s", mqtt_conn ? "OK" : "X");
    display.setTextColor(Synth::YELLOW);
    display.printf(" Sess:%d", count_active_sessions());

    // Heap + Profile
    display.setCursor(4, 78);
    display.setTextColor(Synth::WHITE);
    display.printf("Heap:%lu", (unsigned long)esp_get_free_heap_size());

    display.setCursor(4, 96);
    display.setTextColor(Synth::MAGENTA);
    display.printf("Prof:%s", craw_nvs_active_profile());

    // Navigation hint (size 1)
    display.setTextColor(Synth::TEXT_DIM);
    display.setTextSize(1);
    display.setCursor(4, 122);
    display.print("[A]=Sess  [B]=Settings");
}

// ---------------------------------------------------------------------------
// Drawing: Sessions Dashboard Screen
// ---------------------------------------------------------------------------
static void draw_sessions_screen(void) {
    // Draw to off-screen canvas, then push — zero flicker
    auto &gfx = canvas;
    gfx.fillScreen(Synth::BG);

    int active_count = count_active_sessions();

    // Header (size 2)
    gfx.setTextSize(2);
    gfx.setTextColor(Synth::CYAN);
    gfx.setCursor(4, 2);
    gfx.print("SESSIONS");

    // Animated crawdad in header (scurries when any session is WORKING)
    bool any_working = false;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].active && sessions[i].state == STATE_WORKING) {
            any_working = true;
            break;
        }
    }
    // Draw crawdad to canvas directly
    {
        int cx = 120, cy = 4;
        uint16_t cc = any_working ? Synth::NEON_GREEN : Synth::DIM_CYAN;
        int phase = any_working ? crawdad_leg_phase : 0;
        gfx.fillCircle(cx, cy + 4, 4, cc);
        gfx.fillCircle(cx + 6, cy + 3, 3, cc);
        gfx.fillCircle(cx + 10, cy + 1, 1, Synth::WHITE);
        gfx.fillCircle(cx + 10, cy + 5, 1, Synth::WHITE);
        gfx.drawLine(cx + 9, cy, cx + 13, cy - 2, cc);
        gfx.drawLine(cx + 13, cy - 2, cx + 11, cy - 3, cc);
        gfx.drawLine(cx + 9, cy + 6, cx + 13, cy + 8, cc);
        gfx.drawLine(cx + 13, cy + 8, cx + 11, cy + 9, cc);
        int dy = (phase % 2 == 0) ? 0 : 1;
        gfx.drawLine(cx - 2, cy + 5 + dy, cx - 4, cy + 8 + dy, cc);
        gfx.drawLine(cx + 0, cy + 6 - dy, cx - 2, cy + 9 - dy, cc);
        gfx.drawLine(cx + 2, cy + 6 + dy, cx + 0, cy + 9 + dy, cc);
        gfx.drawLine(cx + 4, cy + 5 - dy, cx + 2, cy + 8 - dy, cc);
    }

    gfx.setTextColor(Synth::TEXT_DIM);
    char countbuf[8];
    snprintf(countbuf, sizeof(countbuf), "%d/%d", active_count, MAX_SESSIONS);
    gfx.setCursor(SCREEN_W - (int)strlen(countbuf) * 12 - 4, 2);
    gfx.print(countbuf);

    // Divider
    gfx.drawLine(4, 20, SCREEN_W - 4, 20, Synth::GRID);

    // Clamp scroll offset
    if (session_scroll_offset >= active_count) {
        session_scroll_offset = 0;
    }
    if (session_scroll_offset < 0) {
        session_scroll_offset = 0;
    }

    int y = 24;
    int active_index = 0;
    int drawn = 0;
    uint32_t now = millis();

    for (int i = 0; i < session_count && i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) continue;

        // Skip rows before scroll offset
        if (active_index < session_scroll_offset) {
            active_index++;
            continue;
        }

        // Stop if we've drawn enough visible rows
        if (drawn >= SESSIONS_VISIBLE_ROWS) break;

        session_t *s = &sessions[i];
        bool stale = session_is_stale(s);

        // --- Row background bar by state ---
        uint16_t row_bg;
        if (stale) {
            row_bg = Synth::DIM_GRAY;
        } else {
            switch (s->state) {
                case STATE_WORKING:    row_bg = rgb(100, 85, 0); break;     // warm amber
                case STATE_NEED_INPUT:
                    row_bg = ((now / 400) % 2 == 0) ? rgb(0, 50, 120) : rgb(0, 25, 60);  // bright blue flash
                    break;
                case STATE_FINISHED:   row_bg = rgb(0, 90, 20); break;     // vivid green
                case STATE_ERROR:
                    row_bg = ((now / 300) % 2 == 0) ? rgb(120, 15, 15) : rgb(50, 5, 5);  // bright red flash
                    break;
                default:               row_bg = Synth::DIM_GRAY; break;
            }
        }

        // Draw background bar
        gfx.fillRect(2, y - 1, SCREEN_W - 24, 18, row_bg);

        gfx.setTextSize(2);
        gfx.setTextColor(Synth::WHITE);

        // Session ID (first 4 chars)
        gfx.setCursor(4, y);
        char id4[5] = {0};
        strncpy(id4, s->id, 4);
        id4[4] = '\0';
        gfx.print(id4);

        // Model (4 chars)
        gfx.setCursor(54, y);
        char mdl[5] = {0};
        truncate_model(s->model, mdl, sizeof(mdl));
        gfx.print(mdl);

        // Timer mm:ss
        uint32_t elapsed = session_timer_elapsed_ms(s);
        int secs = (int)(elapsed / 1000);
        int mm = secs / 60;
        int ss = secs % 60;
        gfx.setCursor(106, y);
        gfx.printf("%02d:%02d", mm, ss);

        // Status letter [W] [F] [I] [N] [E] — color-coded
        {
            uint16_t sc = stale ? Synth::DIM_GRAY : state_color(s->state);
            gfx.setCursor(170, y);
            gfx.setTextColor(sc);
            gfx.printf("[%s]", state_letter(s->state));
        }

        // Crawdad on far right — color-coded per session, animated when WORKING
        if (!stale) {
            uint16_t ccolor = SESSION_COLORS[s->color_index % NUM_SESSION_COLORS];
            bool animate = (s->state == STATE_WORKING);
            int cx = SCREEN_W - 20, cy = y + 2;
            int phase = animate ? crawdad_leg_phase : 0;
            gfx.fillCircle(cx, cy + 4, 4, ccolor);
            gfx.fillCircle(cx + 6, cy + 3, 3, ccolor);
            gfx.fillCircle(cx + 10, cy + 1, 1, Synth::WHITE);
            gfx.fillCircle(cx + 10, cy + 5, 1, Synth::WHITE);
            gfx.drawLine(cx + 9, cy, cx + 13, cy - 2, ccolor);
            gfx.drawLine(cx + 13, cy - 2, cx + 11, cy - 3, ccolor);
            gfx.drawLine(cx + 9, cy + 6, cx + 13, cy + 8, ccolor);
            gfx.drawLine(cx + 13, cy + 8, cx + 11, cy + 9, ccolor);
            int dy = (phase % 2 == 0) ? 0 : 1;
            gfx.drawLine(cx - 2, cy + 5 + dy, cx - 4, cy + 8 + dy, ccolor);
            gfx.drawLine(cx + 0, cy + 6 - dy, cx - 2, cy + 9 - dy, ccolor);
            gfx.drawLine(cx + 2, cy + 6 + dy, cx + 0, cy + 9 + dy, ccolor);
            gfx.drawLine(cx + 4, cy + 5 - dy, cx + 2, cy + 8 - dy, ccolor);
        }

        y += 20;
        drawn++;
        active_index++;
    }

    if (active_count == 0) {
        gfx.setTextSize(2);
        gfx.setTextColor(Synth::TEXT_DIM);
        gfx.setCursor(4, 50);
        gfx.print("No sessions.");
        gfx.setTextSize(1);
        gfx.setCursor(4, 70);
        gfx.print("Waiting for MQTT...");
    }

    // Scroll indicators
    if (active_count > SESSIONS_VISIBLE_ROWS) {
        gfx.setTextSize(1);
        if (session_scroll_offset > 0) {
            gfx.setTextColor(Synth::CYAN);
            gfx.setCursor(SCREEN_W - 10, 24);
            gfx.print("^");
        }
        if (session_scroll_offset + SESSIONS_VISIBLE_ROWS < active_count) {
            gfx.setTextColor(Synth::CYAN);
            gfx.setCursor(SCREEN_W - 10, 112);
            gfx.print("v");
        }
    }

    // Footer hint (size 1)
    gfx.setTextColor(Synth::TEXT_DIM);
    gfx.setTextSize(1);
    gfx.setCursor(4, 122);
    gfx.print("[A]=Scroll  [B]=Back");

    // Push sprite to display in one shot — zero flicker
    canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Drawing: Settings Screen
// ---------------------------------------------------------------------------
static void draw_settings_screen(void) {
    display.fillScreen(Synth::BG);

    // Header (size 2)
    display.setTextSize(2);
    display.setTextColor(Synth::CYAN);
    display.setCursor(4, 2);
    display.print("SETTINGS");

    display.drawLine(4, 20, SCREEN_W - 4, 20, Synth::GRID);

    const char *items[] = { "Sound", "Bright", "Prof", "Clear", "Back" };
    char values[5][16];
    snprintf(values[0], sizeof(values[0]), "%s", sound_enabled ? "ON" : "OFF");
    snprintf(values[1], sizeof(values[1]), "%d%%", brightness_pct);
    // Show active profile name (truncated to fit)
    const char *aprof = craw_nvs_active_profile();
    snprintf(values[2], sizeof(values[2]), "%.7s", aprof ? aprof : "?");
    snprintf(values[3], sizeof(values[3]), "[B]");
    snprintf(values[4], sizeof(values[4]), "<-");

    display.setTextSize(2);
    for (int i = 0; i < SETTINGS_ITEMS; i++) {
        int sy = 24 + i * 18;
        bool selected = (i == settings_cursor);

        // Cursor indicator
        display.setCursor(4, sy);
        display.setTextColor(selected ? Synth::NEON_GREEN : Synth::DIM_GRAY);
        display.print(selected ? ">" : " ");

        // Item text
        display.setCursor(16, sy);
        display.setTextColor(selected ? Synth::WHITE : Synth::TEXT_DIM);
        display.printf("%s:%s", items[i], values[i]);
    }

    // Help text (size 1)
    display.setTextSize(1);
    display.setTextColor(Synth::RED);
    display.setCursor(4, 104);
    display.print("Hold[B] 3s=WiFi clr");

    display.setTextColor(Synth::TEXT_DIM);
    display.setCursor(4, 122);
    display.print("[A]=Next  [B]=Toggle");
}

// ---------------------------------------------------------------------------
// WiFi event callback
// ---------------------------------------------------------------------------
static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    switch (event) {
        case CRAW_WIFI_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected");
            craw_http_start(on_http_notify, on_http_status, nullptr);
            craw_mqtt_start();
            dirty_wifi = true;
            dirty_screen = true;
            break;
        case CRAW_WIFI_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WiFi disconnected");
            dirty_wifi = true;
            dirty_screen = true;
            break;
        case CRAW_WIFI_EVENT_CONNECT_FAILED:
            ESP_LOGW(TAG, "WiFi connect failed");
            dirty_wifi = true;
            dirty_screen = true;
            break;
    }
}

// ---------------------------------------------------------------------------
// MQTT message callback (called for both MQTT and HTTP /notify)
// ---------------------------------------------------------------------------
static void handle_session_message(const craw_mqtt_msg_t *msg) {
    const char *sid = msg->session_id;

    // Use "default" if no session_id in topic
    if (!sid || sid[0] == '\0') {
        sid = "default";
    }

    session_t *s = find_session(sid);
    if (!s) {
        s = create_session(sid);
        if (!s) {
            ESP_LOGW(TAG, "Cannot create session %s (full)", sid);
            return;
        }
        ESP_LOGI(TAG, "New session: %s (color %d)", sid, s->color_index);
    }

    int prev_state = s->state;

    // Update session fields
    s->state = msg->state;
    s->session_pct = msg->session_pct;
    s->last_seen = millis();

    if (msg->model[0]) {
        strncpy(s->model, msg->model, sizeof(s->model) - 1);
        s->model[sizeof(s->model) - 1] = '\0';
    }
    if (msg->client_host[0]) {
        strncpy(s->host, msg->client_host, sizeof(s->host) - 1);
        s->host[sizeof(s->host) - 1] = '\0';
    }

    // Timer management
    switch (s->state) {
        case STATE_WORKING:
            if (prev_state == STATE_IDLE || prev_state == STATE_FINISHED || prev_state == 0) {
                session_timer_reset(s);
            }
            session_timer_start(s);
            break;
        case STATE_NEED_INPUT:
        case STATE_IDLE:
        case STATE_FINISHED:
        case STATE_ERROR:
            session_timer_pause(s);
            break;
    }

    // Chime on state changes
    if (s->state != prev_state) {
        if (s->state == STATE_WORKING) {
            craw_speaker_chime_working();
        } else if (s->state == STATE_NEED_INPUT) {
            craw_speaker_chime_need_input();
        } else if (s->state == STATE_FINISHED) {
            craw_speaker_chime_finished();
        } else if (s->state == STATE_ERROR) {
            craw_speaker_chime_error();
        }
    }

    dirty_sessions = true;
    dirty_screen = true;
}

static void on_mqtt_message(const craw_mqtt_msg_t *msg, void *ctx) {
    handle_session_message(msg);
}

static void on_mqtt_conn(bool connected, void *ctx) {
    ESP_LOGI(TAG, "MQTT %s", connected ? "connected" : "disconnected");
    dirty_wifi = true;
    dirty_screen = true;
}

// ---------------------------------------------------------------------------
// HTTP callbacks
// ---------------------------------------------------------------------------
static void on_http_notify(const craw_mqtt_msg_t *msg, void *ctx) {
    handle_session_message(msg);
}

static char http_status_buf[256];
static const char *on_http_status(void *ctx) {
    snprintf(http_status_buf, sizeof(http_status_buf),
        "{\"device\":\"%s\",\"sessions\":%d,\"wifi\":%s,\"mqtt\":%s,\"heap\":%lu}",
        hostname,
        count_active_sessions(),
        craw_wifi_is_connected() ? "true" : "false",
        craw_mqtt_is_connected() ? "true" : "false",
        (unsigned long)esp_get_free_heap_size());
    return http_status_buf;
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

    // Button A: rising edge (released)
    if (btn_a && !btn_a_last) {
        switch (current_screen) {
            case SCREEN_WIFI:
                current_screen = SCREEN_SESSIONS;
                session_scroll_offset = 0;
                dirty_screen = true;
                break;
            case SCREEN_SESSIONS:
                // Scroll down through sessions
                {
                    int active = count_active_sessions();
                    if (active > SESSIONS_VISIBLE_ROWS) {
                        session_scroll_offset++;
                        if (session_scroll_offset + SESSIONS_VISIBLE_ROWS > active) {
                            session_scroll_offset = 0;  // wrap to top
                        }
                    }
                }
                dirty_screen = true;
                break;
            case SCREEN_SETTINGS:
                // Cycle settings cursor
                settings_cursor = (settings_cursor + 1) % SETTINGS_ITEMS;
                dirty_settings = true;
                dirty_screen = true;
                break;
        }
    }

    // Button B: track press timing for long-press detection
    if (!btn_b && btn_b_last) {
        // Just pressed
        btn_b_press_time = millis();
        btn_b_long_fired = false;
    }

    if (!btn_b && !btn_b_last) {
        // Still held down
        if (!btn_b_long_fired && (millis() - btn_b_press_time) >= 3000) {
            // Long press: clear WiFi + reboot
            btn_b_long_fired = true;
            ESP_LOGW(TAG, "Long press B: clearing WiFi and rebooting");
            craw_serial_printf("Long press B: clearing WiFi credentials and rebooting...\r\n");
            craw_nvs_clear_wifi_creds(wifi_ssid, wifi_pass);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }

    // Button B: rising edge (released) — short press
    if (btn_b && !btn_b_last && !btn_b_long_fired) {
        uint32_t held = millis() - btn_b_press_time;
        if (held < 3000) {
            if (current_screen == SCREEN_SETTINGS) {
                // Toggle/activate selected item
                switch (settings_cursor) {
                    case 0: // Sound
                        sound_enabled = !sound_enabled;
                        craw_speaker_set_sound_enabled(sound_enabled);
                        craw_nvs_save_sound_pref(sound_enabled);
                        if (sound_enabled) {
                            craw_speaker_chime_working();
                        }
                        break;
                    case 1: // Brightness
                        brightness_pct += 20;
                        if (brightness_pct > 100) brightness_pct = 20;
                        {
                            int mapped = (brightness_pct * 255) / 100;
                            display.setBrightness(mapped);
                        }
                        break;
                    case 2: // Profile — cycle to next profile and reconnect
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
                                // Load new profile creds and reconnect
                                craw_nvs_load_profile_creds(next_name, wifi_ssid, wifi_pass);
                                craw_serial_printf("[PROF] Switched to '%s'\r\n", next_name);
                                if (wifi_ssid[0]) {
                                    craw_wifi_connect(wifi_ssid, wifi_pass);
                                }
                                dirty_wifi = true;
                            }
                        }
                        break;
                    case 3: // Clear sessions
                        clear_all_sessions();
                        break;
                    case 4: // Back
                        current_screen = SCREEN_WIFI;
                        settings_cursor = 0;
                        break;
                }
                dirty_settings = true;
                dirty_screen = true;
            } else if (current_screen == SCREEN_SESSIONS) {
                // Sessions -> back to WiFi
                current_screen = SCREEN_WIFI;
                dirty_screen = true;
            } else {
                // WiFi -> go to Settings
                current_screen = SCREEN_SETTINGS;
                dirty_screen = true;
            }
        }
    }

    btn_a_last = btn_a;
    btn_b_last = btn_b;
}

// ---------------------------------------------------------------------------
// Session staleness check
// ---------------------------------------------------------------------------
static void check_session_staleness(void) {
    uint32_t now = millis();
    bool changed = false;

    for (int i = session_count - 1; i >= 0; i--) {
        if (!sessions[i].active) continue;

        uint32_t age = now - sessions[i].last_seen;
        if (age > REMOVE_TIMEOUT_MS) {
            ESP_LOGI(TAG, "Removing expired session %s", sessions[i].id);
            remove_session(i);
            changed = true;
        }
    }

    if (changed) {
        dirty_sessions = true;
        dirty_screen = true;
    }
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
    if (wifi_pass[0] == '\0')
        craw_serial_print("No password (open network)\r\n");
    else
        craw_serial_print("Password set.\r\n");
}

static void w_wifi_connect(void) {
    if (wifi_ssid[0] == '\0') {
        craw_serial_print("No SSID set. Use wifi-ssid first.\r\n");
        return;
    }
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

    int cnt = 0;
    auto list = craw_nvs_profile_list(&cnt);
    craw_serial_print("Profiles: ");
    for (int i = 0; i < cnt; i++) {
        bool active = (strcmp(list[i], craw_nvs_active_profile()) == 0);
        craw_serial_printf("%s%s%s ", active ? "[" : "", list[i], active ? "]" : "");
    }
    craw_serial_print("\r\n");

    char ip_buf[20] = "N/A";
    if (craw_wifi_is_connected()) {
        craw_wifi_get_ip_str(ip_buf, sizeof(ip_buf));
    }
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
    dirty_wifi = true;
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
    if (input[0] == '\0') {
        craw_serial_print("Cancelled.\r\n");
        return;
    }

    // Check if numeric
    int n = atoi(input);
    if (n >= 1 && n <= cnt) {
        craw_nvs_set_active_profile(list[n - 1]);
        craw_nvs_load_wifi_creds(wifi_ssid, wifi_pass);
        craw_serial_printf("Switched to profile '%s'\r\n", craw_nvs_active_profile());
        return;
    }

    // Name-based lookup
    if (craw_nvs_profile_find(input) >= 0) {
        craw_nvs_set_active_profile(input);
        craw_nvs_load_wifi_creds(wifi_ssid, wifi_pass);
        craw_serial_printf("Switched to profile '%s'\r\n", input);
        return;
    }

    // New profile
    if (craw_nvs_profile_count() >= CRAW_PROFILE_MAX_COUNT) {
        craw_serial_printf("Maximum profiles (%d) reached.\r\n", CRAW_PROFILE_MAX_COUNT);
        return;
    }
    if (!craw_nvs_profile_name_valid(input)) {
        craw_serial_print("Invalid profile name.\r\n");
        return;
    }
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
            craw_mqtt_init(mqtt_broker, mqtt_topic, on_mqtt_message, on_mqtt_conn, nullptr);
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

static void w_status(void) {
    craw_serial_printf("Sessions: %d active\r\n", count_active_sessions());
    for (int i = 0; i < session_count; i++) {
        if (!sessions[i].active) continue;
        session_t *s = &sessions[i];
        uint32_t elapsed = session_timer_elapsed_ms(s) / 1000;
        craw_serial_printf("  %s: state=%s model=%s pct=%d%% time=%d:%02d host=%s\r\n",
            s->id, state_letter(s->state), s->model,
            s->session_pct, (int)(elapsed / 60), (int)(elapsed % 60),
            s->host[0] ? s->host : "(none)");
    }
    craw_serial_printf("Sound:   %s\r\n", sound_enabled ? "on" : "off");
    craw_serial_printf("Bright:  %d%%\r\n", brightness_pct);
    craw_serial_printf("Heap:    %lu\r\n", (unsigned long)esp_get_free_heap_size());
}

static void w_brightness(void) {
    int n = (int)forth_pop();
    if (n < 0) n = 0;
    if (n > 100) n = 100;
    brightness_pct = n;
    int mapped = (brightness_pct * 255) / 100;
    display.setBrightness(mapped);
    craw_serial_printf("Brightness: %d%%\r\n", brightness_pct);
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
    forth_register_word("status",        w_status);
    forth_register_word("brightness",    w_brightness);
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
extern "C" void app_main(void)
{
    // 1. Serial init
    craw_serial_init();
    vTaskDelay(pdMS_TO_TICKS(300));

    // 2. Banner
    craw_serial_print("\r\n\r\n");
    craw_serial_print("============================================\r\n");
    craw_serial_printf("  Crawdad OpenWR v%s\r\n", VERSION_STRING);
    craw_serial_printf("  Forth %s | Build: %s %s\r\n",
        ESPIDFORTH_VERSION_STRING, ESPIDFORTH_BUILD_DATE, ESPIDFORTH_BUILD_TIME);
    craw_serial_print("  Multi-Session Claude Code Monitor\r\n");
    craw_serial_print("============================================\r\n");

    // 3. Display init (M5GFX auto-detects M5StickC Plus, handles AXP192)
    display.init();
    display.setRotation(1);  // landscape: 240 wide x 135 tall, USB on right
    display.fillScreen(Synth::BG);
    display.setTextWrap(false);
    canvas.createSprite(SCREEN_W, SCREEN_H);  // off-screen buffer for flicker-free drawing
    canvas.setTextWrap(false);
    int mapped = (brightness_pct * 255) / 100;
    display.setBrightness(mapped);
    craw_serial_print("Display initialized.\r\n");

    // 4. Speaker init (GPIO 2)
    craw_speaker_init(SPEAKER_GPIO);

    // 5. Button init
    button_init();

    // 6. NVS init
    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();
    craw_nvs_profiles_load();

    // Load sound preference
    sound_enabled = craw_nvs_load_sound_pref();
    craw_speaker_set_sound_enabled(sound_enabled);
    craw_serial_printf("Sound: %s\r\n", sound_enabled ? "on" : "off");

    // Load MQTT broker
    craw_nvs_load_mqtt_broker(mqtt_broker, sizeof(mqtt_broker));
    craw_serial_printf("MQTT broker: %s\r\n", mqtt_broker);

    // 7. Hostname derivation
    derive_hostname();
    derive_mqtt_topic();
    craw_serial_printf("MQTT topic: %s\r\n", mqtt_topic);

    // 8. WiFi init
    craw_wifi_init(hostname, on_wifi_event, nullptr);

    // 9. MQTT init (does not connect until WiFi is up)
    craw_mqtt_init(mqtt_broker, mqtt_topic, on_mqtt_message, on_mqtt_conn, nullptr);

    // 10. Forth init + register words
    forth_init(FORTH_HEAP_SIZE);
    register_forth_words();
    craw_serial_print("Forth engine initialized.\r\n");
    craw_serial_printf("Free heap: %lu bytes\r\n\r\n",
        (unsigned long)esp_get_free_heap_size());

    craw_serial_print("Forth commands:\r\n");
    craw_serial_print("  N brightness  -- set brightness (0-100)\r\n");
    craw_serial_print("  wifi-ssid     -- set WiFi SSID\r\n");
    craw_serial_print("  wifi-pass     -- set WiFi password\r\n");
    craw_serial_print("  wifi-connect  -- connect and save\r\n");
    craw_serial_print("  wifi-status   -- show connection info\r\n");
    craw_serial_print("  wifi-clear    -- erase credentials\r\n");
    craw_serial_print("  wifi-profiles -- manage WiFi profiles\r\n");
    craw_serial_print("  mqtt-broker   -- view/change MQTT broker\r\n");
    craw_serial_print("  mqtt-status   -- show MQTT state\r\n");
    craw_serial_print("  mqtt-topic    -- print subscription topic\r\n");
    craw_serial_print("  sound-on      -- enable chimes\r\n");
    craw_serial_print("  sound-off     -- disable chimes\r\n");
    craw_serial_print("  status        -- print session info\r\n\r\n");

    // 11. Start Forth REPL task (background)
    xTaskCreate(forth_repl_task, "forth_repl", 8192, NULL, 3, NULL);

    // 12. Draw initial screen (WiFi status)
    current_screen = SCREEN_WIFI;
    draw_wifi_screen();
    dirty_screen = false;

    // 13. Auto-connect WiFi from NVS
    if (craw_nvs_load_wifi_creds(wifi_ssid, wifi_pass)) {
        craw_serial_printf("Stored WiFi: '%s' -- connecting...\r\n", wifi_ssid);
        craw_wifi_connect(wifi_ssid, wifi_pass);
    } else {
        craw_serial_print("No stored WiFi. Configure via Forth REPL.\r\n");
    }

    // Initialize session array
    memset(sessions, 0, sizeof(sessions));

    // 14. Main loop
    uint32_t last_staleness_check = millis();
    uint32_t last_anim_tick = millis();

    while (1) {
        uint32_t now = millis();

        // Button polling
        handle_buttons();

        // Crawdad animation + flashing state tick (every 150ms)
        if (now - last_anim_tick >= 150) {
            crawdad_leg_phase = (crawdad_leg_phase + 1) % 4;
            last_anim_tick = now;

            if (current_screen == SCREEN_WIFI) {
                // Clear just the crawdad area and redraw it
                display.fillRect(SCREEN_W - 32, 2, 30, 16, Synth::BG);
                draw_crawdad_mini(SCREEN_W - 30, 4, Synth::HOT_PINK, true);
            } else if (current_screen == SCREEN_SESSIONS) {
                // Check if any session needs flashing (NEED_INPUT or ERROR)
                // or has an animated crawdad (any active session)
                // Use a full redraw — at 150ms interval and fillScreen, this is
                // acceptably smooth for the small 240x135 display
                dirty_sessions = true;
                dirty_screen = true;
            }
        }

        // Session timer updates (every 1s) for sessions screen
        if (current_screen == SCREEN_SESSIONS) {
            static uint32_t last_timer_update = 0;
            if (now - last_timer_update >= 1000) {
                for (int i = 0; i < session_count; i++) {
                    if (sessions[i].active && sessions[i].timer_running) {
                        dirty_screen = true;
                        break;
                    }
                }
                last_timer_update = now;
            }
        }

        // Session staleness check (every 10s)
        if (now - last_staleness_check >= STALENESS_CHECK_MS) {
            check_session_staleness();
            last_staleness_check = now;
        }

        // Screen drawing
        if (dirty_screen) {
            switch (current_screen) {
                case SCREEN_WIFI:
                    draw_wifi_screen();
                    break;
                case SCREEN_SESSIONS:
                    draw_sessions_screen();
                    break;
                case SCREEN_SETTINGS:
                    draw_settings_screen();
                    break;
            }
            dirty_screen = false;
        }

        // Speaker update (manages tone durations)
        craw_speaker_update();

        // Yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
