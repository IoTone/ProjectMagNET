/*
 * XIAO ESP32C3 IOT Lighting — E4TH (Phase 1: port + self-test)
 *
 * Port of the campsynthesis 4-strip random-color NeoPixel sketch to the
 * ESPIDFORTH ("E4TH") runtime, on the Xiao ESP32C3 in a Grove Shield for
 * XIAO. The strips are Grove WS2813 (60 px each).
 *
 * Phase 1 drives ONE strip on the Grove D0 socket via the ESP32-C3 RMT
 * peripheral (ESP-IDF led_strip) — hardware-timed, so no bit-bang jitter.
 * WS2813 has a dual data line (DIN + backup BIN); the D0 socket exposes
 * DIN on GPIO2 (yellow) and BIN on GPIO3 (white). Both are driven with
 * identical pixels using the C3's two RMT TX channels. Driving 4 fully
 * independent strips needs >2 channels — that topology (likely chaining
 * the WS2813s into one run) is a Phase-2 decision.
 *
 * The XIAO ESP32C3 does NOT break out raw GPIO 0/1; its silkscreen pads
 * D0..D5 are GPIO 2,3,4,5,6,7. NOTE: WS2813 needs ~5V VDD — a 3.3V Grove
 * socket will not light the strip regardless of firmware.
 *
 * Phase 1 is scaffold + port + self-test only. No WiFi/BLE/HTTP — those
 * arrive in Phase 2 (BLE->WiFi provisioning + UC2 /api/v1/actuator/neopixel).
 *
 * REPL words (type at the USB-serial console):
 *   selftest       -- bars + all-on + per-strip walk (verifies wiring/order)
 *   selftest-bars  -- fill all strips R, then G, then B (channel-order check)
 *   selftest-allon -- all strips dim white for 3 s
 *   selftest-walk  -- single pixel marches down each strip in turn
 *   N randrun      -- N frames of the ported random-color pattern
 *   ws-clear       -- blank all strips
 *   strips         -- print wiring / word help
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "driver/usb_serial_jtag.h"
#include "led_strip.h"
#include "mdns.h"
#include "craw_nvs.h"
#include "craw_wifi.h"
#include "craw_ble_provision.h"
#include "strip_ctl.h"
#include "http_strip.h"
#include "forth_core.h"
#include "forth_version.h"

/* mDNS hostname uses the same MAC-suffix the BLE name does, so the device
 * has ONE identifier you read in nRF Connect and reuse for HTTP / smoke
 * test (e.g. `magnet-lighting-b7c0.local`). Static buffer filled at boot. */
#define MDNS_HOSTNAME_PREFIX "magnet-lighting"
static char s_mdns_hostname[24] = MDNS_HOSTNAME_PREFIX;
#define MDNS_INSTANCE  "MagNET Lighting"

#define STRIP_COUNT 4
#define NUM_PIXELS  60
#define BYTES_PER   (NUM_PIXELS * 3)
/* Trimmed from 64 KB: NimBLE controller needs a ~40 KB contiguous internal
 * DRAM block, and a 64 KB Forth heap left so little contiguous space that
 * nimble_port_init failed silently (no `advertising as ...` ESP_LOGI fires).
 * 32 KB is plenty for ESP32forth core + our bundle. */
#define FORTH_HEAP_SIZE (32 * 1024)

/* Phase 1: ONE physical Grove WS2813 strip on the D0 socket, driven by the
 * ESP32-C3 RMT peripheral (hardware-timed -- no bit-bang jitter). WS2813
 * has a dual data line (DIN + backup BIN); the Grove D0 socket exposes DIN
 * on GPIO2 (yellow) and BIN on GPIO3 (white). Both must carry the data, so
 * we use the C3's two RMT TX channels: one led_strip device per pin, fed
 * identical pixels. (4 independent strips don't fit 2 RMT channels -- the
 * multi-strip topology, likely chaining, is a Phase-2 decision.)
 *
 * Logical strips 1..3 (NSTRIP stays 4 so the Forth bundle is unchanged)
 * have no hardware in Phase 1 and are no-ops on show. */
#define STRIP_DIN_GPIO 2   /* Grove D0 primary pin (yellow, WS2813 DIN) */
#define STRIP_BIN_GPIO 3   /* Grove D0 2nd pin     (white,  WS2813 BIN) */

static led_strip_handle_t s_din;   /* RMT device on DIN (GPIO2) */
static led_strip_handle_t s_bin;   /* RMT device on BIN (GPIO3) */

/* Per-strip shadow framebuffer, WS2812 wire order = G,R,B. */
static uint8_t s_fb[STRIP_COUNT][BYTES_PER];

/* Master brightness cap (0..255), applied per channel. Default 64 (~25%)
 * so a stray "full white on all strips" can't demand ~14 A on a bench PSU.
 * Raise with `255 bri` once an adequately-sized 5V supply is wired. */
static volatile uint8_t s_bri_cap = 64;

/* Active pixels per strip (1..NUM_PIXELS). Lets a partial / cut strip
 * self-test cleanly: fills and the walk only touch connected LEDs.
 * Set with `30 npx`. */
static volatile int s_active_px = NUM_PIXELS;

/* ---- Phase 2: BLE/WiFi provisioning + HTTP-driven strip state ---- */

static volatile int s_prov_state = 0;          /* mirrors craw_ble_prov_state_t */
static char  s_ip_str[20] = "N/A";
static bool  s_mdns_started = false;

const char *const STRIP_PATTERN_NAMES[PAT_COUNT] = {
    "solid", "breathing", "rainbow", "chase", "twinkle",
};

static SemaphoreHandle_t s_strip_mtx;
static strip_state_t s_strip = {
    .on = false, .brightness_pct = 60,
    .r = 0, .g = 200, .b = 255,
    .pattern = PAT_SOLID, .pattern_speed_pct = 50,
    .led_count = NUM_PIXELS, .last_changed_us = 0,
};
/* Three-way ownership of the strip's pixels.
 *  STATUS: render the BLE/WiFi provisioning status pattern (boot default —
 *          gives the installer visual feedback during bring-up).
 *  FORTH:  Forth REPL owns the strip; the task does not touch it (use
 *          `strip-release` to enter this state for self-test).
 *  HTTP:   render the HTTP-driven `s_strip` state (set automatically on
 *          the first `strip_set()` call, or manually via `strip-engage`). */
typedef enum { OWNER_STATUS = 0, OWNER_FORTH = 1, OWNER_HTTP = 2 } strip_owner_t;
static volatile strip_owner_t s_owner = OWNER_STATUS;

/* BLE-write ack: a transient white pulse on the strip whenever a BLE
 * characteristic write fires (creds received / commit requested), so the
 * installer can see the device accepted the provisioning packet. */
static volatile int64_t s_ack_until_us = 0;

/* ---- USB serial I/O ---- */

static void usb_print(const char *s) {
    usb_serial_jtag_write_bytes((const uint8_t *)s, strlen(s), pdMS_TO_TICKS(500));
}

static void usb_printf(const char *fmt, ...) {
    char buf[200];
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

/* ---- WS2813 RMT driver (ESP-IDF led_strip) ---- */

static led_strip_handle_t make_rmt_strip(int gpio) {
    led_strip_config_t scfg = {
        .strip_gpio_num = gpio,
        .max_leds = NUM_PIXELS,
    };
    led_strip_rmt_config_t rcfg = {
        .resolution_hz = 10 * 1000 * 1000,   /* 10 MHz -> WS2812/13 timing */
    };
    led_strip_handle_t h = NULL;
    esp_err_t e = led_strip_new_rmt_device(&scfg, &rcfg, &h);
    if (e != ESP_OK) {
        usb_printf("[led] RMT init GPIO %d failed: %d\r\n", gpio, (int)e);
        return NULL;
    }
    led_strip_clear(h);
    return h;
}

static void ws_gpio_init(void) {
    memset(s_fb, 0, sizeof(s_fb));
    s_din = make_rmt_strip(STRIP_DIN_GPIO);
    s_bin = make_rmt_strip(STRIP_BIN_GPIO);   /* WS2813 backup line */
    if (!s_bin)
        usb_print("[led] BIN channel unavailable -- driving DIN only\r\n");
}

static inline int strip_ok(int s) { return s >= 0 && s < STRIP_COUNT; }

static inline uint8_t bri_scale(uint8_t v) {
    return (uint8_t)(((uint32_t)v * s_bri_cap) / 255U);
}

static void ws_set_pixel(int s, int i, uint8_t r, uint8_t g, uint8_t b) {
    if (!strip_ok(s) || i < 0 || i >= NUM_PIXELS) return;
    uint8_t *p = &s_fb[s][i * 3];
    p[0] = bri_scale(g); p[1] = bri_scale(r); p[2] = bri_scale(b);  /* GRB */
}

static void ws_fill(int s, uint8_t r, uint8_t g, uint8_t b) {
    if (!strip_ok(s)) return;
    for (int i = 0; i < NUM_PIXELS; i++)
        if (i < s_active_px) ws_set_pixel(s, i, r, g, b);
        else                 ws_set_pixel(s, i, 0, 0, 0);
}

static void ws_rand_fill(int s) {
    if (!strip_ok(s)) return;
    for (int i = 0; i < NUM_PIXELS; i++) {
        if (i < s_active_px) {
            uint32_t v = esp_random();
            ws_set_pixel(s, i, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        } else {
            ws_set_pixel(s, i, 0, 0, 0);
        }
    }
}

/* Push strip 0's shadow framebuffer to the RMT device(s) and latch. RMT
 * generates WS2813 timing in hardware. Phase 1 has one physical strip
 * (logical 0); other indices are no-ops. */
static void ws_show(int s) {
    if (s != 0 || !s_din) return;
    for (int i = 0; i < NUM_PIXELS; i++) {
        uint8_t *p = &s_fb[0][i * 3];   /* stored G,R,B */
        led_strip_set_pixel(s_din, i, p[1], p[0], p[2]);
        if (s_bin) led_strip_set_pixel(s_bin, i, p[1], p[0], p[2]);
    }
    led_strip_refresh(s_din);
    if (s_bin) led_strip_refresh(s_bin);
}

static void ws_show_all(void) {
    ws_show(0);
}

static void ws_clear_all(void) {
    memset(s_fb, 0, sizeof(s_fb));
    if (s_din) led_strip_clear(s_din);
    if (s_bin) led_strip_clear(s_bin);
}

/* ---- Phase 2: HTTP-driven render task + control surface ---- */

static const char *TAG = "lighting";

int strip_pattern_from_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < PAT_COUNT; i++)
        if (strcmp(name, STRIP_PATTERN_NAMES[i]) == 0) return i;
    return -1;
}

void strip_get_state(strip_state_t *out) {
    xSemaphoreTake(s_strip_mtx, portMAX_DELAY);
    *out = s_strip;
    xSemaphoreGive(s_strip_mtx);
}

void strip_set(const bool *on, const int *bri, const uint8_t rgb[3],
               const strip_pattern_t *pat, const int *spd) {
    xSemaphoreTake(s_strip_mtx, portMAX_DELAY);
    if (on)  s_strip.on = *on;
    if (bri) s_strip.brightness_pct = *bri < 0 ? 0 : *bri > 100 ? 100 : *bri;
    if (rgb) { s_strip.r = rgb[0]; s_strip.g = rgb[1]; s_strip.b = rgb[2]; }
    if (pat) s_strip.pattern = *pat;
    if (spd) s_strip.pattern_speed_pct = *spd < 0 ? 0 : *spd > 100 ? 100 : *spd;
    s_strip.last_changed_us = esp_timer_get_time();
    xSemaphoreGive(s_strip_mtx);
    s_owner = OWNER_HTTP;   /* HTTP now owns the strip */
}

static void hsv2rgb(int h, uint8_t *r, uint8_t *g, uint8_t *b) {
    h %= 360; if (h < 0) h += 360;
    int region = h / 60, rem = (h - region * 60) * 255 / 60;
    int p = 0, q = 255 - rem, t = rem, v = 255;
    switch (region) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default:*r = v; *g = p; *b = q; break;
    }
}

static void render_frame(const strip_state_t *s, uint32_t f) {
    if (!s->on) {
        for (int i = 0; i < NUM_PIXELS; i++) ws_set_pixel(0, i, 0, 0, 0);
        ws_show(0);
        return;
    }
    /* brightness_pct drives the existing per-channel cap; the pattern owns
     * its own temporal/spatial shaping of the base color on top of that. */
    s_bri_cap = (uint8_t)(s->brightness_pct * 255 / 100);
    int spd = s->pattern_speed_pct;            /* 0..100 */

    switch (s->pattern) {
    case PAT_SOLID:
        for (int i = 0; i < NUM_PIXELS; i++) ws_set_pixel(0, i, s->r, s->g, s->b);
        break;
    case PAT_BREATHING: {
        float hz = 0.15f + spd * 0.0185f;       /* ~0.15..2 Hz */
        float ph = (float)f * 0.02f * hz * 6.2832f;
        float k  = 0.10f + 0.90f * (0.5f - 0.5f * cosf(ph));
        uint8_t r = (uint8_t)(s->r * k), g = (uint8_t)(s->g * k),
                b = (uint8_t)(s->b * k);
        for (int i = 0; i < NUM_PIXELS; i++) ws_set_pixel(0, i, r, g, b);
        break;
    }
    case PAT_RAINBOW: {
        int adv = (f * (1 + spd / 4)) % 360;
        for (int i = 0; i < NUM_PIXELS; i++) {
            uint8_t r, g, b;
            hsv2rgb(i * 360 / NUM_PIXELS + adv, &r, &g, &b);
            ws_set_pixel(0, i, r, g, b);
        }
        break;
    }
    case PAT_CHASE: {
        int head = (f * (1 + spd / 12)) % NUM_PIXELS;
        for (int i = 0; i < NUM_PIXELS; i++) {
            int d = (i - head + NUM_PIXELS) % NUM_PIXELS;
            if (d < 6) {
                int k = 255 - d * 42;
                ws_set_pixel(0, i, s->r * k / 255, s->g * k / 255, s->b * k / 255);
            } else {
                ws_set_pixel(0, i, 0, 0, 0);
            }
        }
        break;
    }
    case PAT_TWINKLE: {
        static uint8_t tw[NUM_PIXELS];
        for (int i = 0; i < NUM_PIXELS; i++) tw[i] = (uint8_t)(tw[i] * 7 / 8);
        int ignite = 1 + spd / 15;
        for (int n = 0; n < ignite; n++)
            tw[esp_random() % NUM_PIXELS] = 255;
        for (int i = 0; i < NUM_PIXELS; i++) {
            int k = tw[i];
            ws_set_pixel(0, i, s->r * k / 255, s->g * k / 255, s->b * k / 255);
        }
        break;
    }
    default: break;
    }
    ws_show(0);
}

/* Provisioning-status renderer — the BLE/WiFi UX shown on the strip when
 * neither HTTP nor the Forth REPL has claimed it.
 *
 * Standard hive node UX language:
 *   IDLE (no creds, advertising)     orange flash (~1 Hz)
 *   CREDS/COMMIT/CONNECTING          orange faster flash (~3 Hz)
 *   CONNECTED                        green breathing (~0.5 Hz)
 *   FAILED                           red strobe (~8 Hz)
 *   BLE characteristic write         white pulse (~250 ms) overrides above */
static void render_status(uint32_t f) {
    s_bri_cap = 48;   /* USB-safe full-strip brightness */
    int64_t now = esp_timer_get_time();
    if (now < s_ack_until_us) {
        for (int i = 0; i < NUM_PIXELS; i++) ws_set_pixel(0, i, 255, 255, 255);
        ws_show(0);
        return;
    }
    uint8_t r = 0, g = 0, b = 0;
    bool lit = true;
    switch (s_prov_state) {
    case CRAW_BLE_PROV_FAILED:
        lit = ((f / 3) & 1);   /* 3 frames * 20ms ≈ 60ms on/60ms off, ~8 Hz */
        r = 255; g = 0; b = 0;
        break;
    case CRAW_BLE_PROV_CONNECTED: {
        float ph = (float)f * 0.02f * 6.2832f * 0.4f;   /* ~0.4 Hz breath */
        float k  = 0.10f + 0.90f * (0.5f - 0.5f * cosf(ph));
        r = 0; g = (uint8_t)(255 * k); b = 0;
        break;
    }
    case CRAW_BLE_PROV_CREDS_RECEIVED:
    case CRAW_BLE_PROV_COMMIT_REQUESTED:
    case CRAW_BLE_PROV_CONNECTING:
        lit = ((f / 8) & 1);   /* 8 frames * 20ms ≈ 160ms on/off, ~3 Hz */
        r = 255; g = 80; b = 0;
        break;
    case CRAW_BLE_PROV_IDLE:
    default:
        lit = ((f / 25) & 1);  /* 25 frames * 20ms = 500ms on/off, 1 Hz */
        r = 255; g = 80; b = 0;
        break;
    }
    if (lit) for (int i = 0; i < NUM_PIXELS; i++) ws_set_pixel(0, i, r, g, b);
    else     for (int i = 0; i < NUM_PIXELS; i++) ws_set_pixel(0, i, 0, 0, 0);
    ws_show(0);
}

static void strip_task(void *arg) {
    (void)arg;
    uint32_t f = 0;
    for (;;) {
        switch (s_owner) {
        case OWNER_HTTP: {
            strip_state_t s;
            strip_get_state(&s);
            render_frame(&s, f);
            break;
        }
        case OWNER_STATUS:
            render_status(f);
            break;
        case OWNER_FORTH:
        default:
            /* leave the strip alone; Forth REPL owns the pixels */
            break;
        }
        f++;
        vTaskDelay(pdMS_TO_TICKS(20));   /* ~50 fps */
    }
}

void strip_ctl_init(void) {
    s_strip_mtx = xSemaphoreCreateMutex();
    xTaskCreate(strip_task, "strip", 4096, NULL, 4, NULL);
}

/* ---- mDNS (init -> hostname -> instance -> service -> txt) ---- */

static void start_mdns(void) {
    if (s_mdns_started) return;
    /* Derive the same MAC suffix used by the BLE GAP name so the device
     * has a single identifier across BLE and mDNS. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mdns_hostname, sizeof(s_mdns_hostname), "%s-%02x%02x",
             MDNS_HOSTNAME_PREFIX, mac[4], mac[5]);

    if (mdns_init() != ESP_OK) { ESP_LOGW(TAG, "mdns_init failed"); return; }
    if (mdns_hostname_set(s_mdns_hostname) != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set failed"); return;
    }
    mdns_instance_name_set(MDNS_INSTANCE);
    mdns_txt_item_t txt[] = { { "role", "lighting" }, { "model", "WS2813" } };
    if (mdns_service_add(NULL, "_http", "_tcp", 80, txt, 2) != ESP_OK) {
        ESP_LOGW(TAG, "mdns_service_add failed"); return;
    }
    s_mdns_started = true;
    usb_printf("[mDNS] http://%s.local/ on the LAN\r\n", s_mdns_hostname);
}

/* ---- WiFi + BLE provisioning callbacks ---- */

static void on_wifi_event(craw_wifi_event_t event, void *ctx) {
    (void)ctx;
    switch (event) {
    case CRAW_WIFI_EVENT_CONNECTED:
        craw_wifi_get_ip_str(s_ip_str, sizeof(s_ip_str));
        usb_printf("\r\n[WiFi] connected, IP: %s\r\n", s_ip_str);
        craw_ble_provision_set_ip(s_ip_str);
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTED);
        s_prov_state = CRAW_BLE_PROV_CONNECTED;
        start_mdns();
        if (http_strip_start() == ESP_OK)
            usb_printf("[HTTP] http://%s/api/v1/actuator/neopixel\r\n", s_ip_str);
        break;
    case CRAW_WIFI_EVENT_DISCONNECTED:
        usb_print("\r\n[WiFi] disconnected\r\n");
        strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
        craw_ble_provision_set_ip(s_ip_str);
        http_strip_stop();
        break;
    case CRAW_WIFI_EVENT_CONNECT_FAILED:
        usb_print("\r\n[WiFi] connect failed\r\n");
        strncpy(s_ip_str, "N/A", sizeof(s_ip_str));
        craw_ble_provision_set_ip(s_ip_str);
        craw_ble_provision_set_status(CRAW_BLE_PROV_FAILED);
        s_prov_state = CRAW_BLE_PROV_FAILED;
        break;
    }
}

static void on_prov_event(craw_ble_prov_state_t state,
                          const char *ssid, const char *pass, void *ctx) {
    (void)ctx;
    s_prov_state = (int)state;
    /* Acknowledge every BLE characteristic write with a brief white pulse
     * (rendered by render_status() when the strip is in OWNER_STATUS). */
    s_ack_until_us = esp_timer_get_time() + 250000;
    switch (state) {
    case CRAW_BLE_PROV_CREDS_RECEIVED:
        usb_printf("\r\n[PROV] creds: ssid='%s' pass=%d chars\r\n",
                   ssid, (int)strlen(pass ? pass : ""));
        break;
    case CRAW_BLE_PROV_COMMIT_REQUESTED:
        if (!ssid || !ssid[0]) {
            usb_print("\r\n[PROV] empty SSID, ignoring\r\n");
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

/* ---- Forth FFI words ---- */

/* ( strip idx r g b -- ) */
static void w_ws_px(void) {
    uint8_t b = (uint8_t)forth_pop();
    uint8_t g = (uint8_t)forth_pop();
    uint8_t r = (uint8_t)forth_pop();
    int idx   = (int)forth_pop();
    int s     = (int)forth_pop();
    ws_set_pixel(s, idx, r, g, b);
}

/* ( strip r g b -- ) */
static void w_ws_fill(void) {
    uint8_t b = (uint8_t)forth_pop();
    uint8_t g = (uint8_t)forth_pop();
    uint8_t r = (uint8_t)forth_pop();
    int s     = (int)forth_pop();
    ws_fill(s, r, g, b);
}

/* ( r g b -- ) */
static void w_ws_fill_all(void) {
    uint8_t b = (uint8_t)forth_pop();
    uint8_t g = (uint8_t)forth_pop();
    uint8_t r = (uint8_t)forth_pop();
    for (int s = 0; s < STRIP_COUNT; s++) ws_fill(s, r, g, b);
}

/* ( strip -- ) */
static void w_ws_rand_fill(void) {
    ws_rand_fill((int)forth_pop());
}

/* ( strip -- ) */
static void w_ws_show(void) {
    ws_show((int)forth_pop());
}

/* ( -- ) */
static void w_ws_show_all(void) {
    ws_show_all();
}

/* ( -- ) */
static void w_ws_clear(void) {
    ws_clear_all();
}

/* ( n -- )  set master brightness cap 0..255 */
static void w_bri(void) {
    intptr_t n = forth_pop();
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    s_bri_cap = (uint8_t)n;
    usb_printf("\r\nbrightness cap = %d/255\r\n", (int)s_bri_cap);
}

/* ( -- n )  push current brightness cap */
static void w_bri_q(void) {
    forth_push(s_bri_cap);
}

/* ( n -- )  set active pixels per strip 1..NUM_PIXELS (partial strips) */
static void w_npx(void) {
    intptr_t n = forth_pop();
    if (n < 1) n = 1;
    if (n > NUM_PIXELS) n = NUM_PIXELS;
    s_active_px = (int)n;
    usb_printf("\r\nactive pixels/strip = %d\r\n", s_active_px);
}

/* ( -- n )  push active pixels per strip */
static void w_npx_q(void) {
    forth_push(s_active_px);
}

/* ( n -- r )  pseudo-random in [0,n) */
static void w_rnd(void) {
    intptr_t n = forth_pop();
    forth_push(n > 0 ? (intptr_t)(esp_random() % (uint32_t)n) : 0);
}

/* ( ms -- )  cooperative delay */
static void w_nap(void) {
    intptr_t ms = forth_pop();
    if (ms < 0) ms = 0;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ( -- )  print wiring + word help */
static void w_strips(void) {
    usb_print("\r\nGrove WS2813, 60 px, RMT-driven (Phase 1: 1 strip):\r\n");
    usb_print("  strip 0 -> Grove D0: GPIO2 DIN + GPIO3 BIN (2x RMT)\r\n");
    usb_print("  strips 1-3 -> no hardware in Phase 1 (no-op)\r\n");
    usb_print("  (WS2813 needs ~5V VDD; Grove socket VCC may be 3.3V)\r\n");
    usb_print("Words:\r\n");
    usb_print("  smoke           low-power: px0 of each strip dim (USB-safe)\r\n");
    usb_print("  N id            light first 3 px of strip N dim green\r\n");
    usb_print("  selftest        bars + all-on + walk\r\n");
    usb_print("  selftest-bars   R / G / B fill (channel order)\r\n");
    usb_print("  selftest-allon  all strips dim white 3 s\r\n");
    usb_print("  selftest-walk   marching pixel per strip\r\n");
    usb_print("  N randrun       N frames of random pattern\r\n");
    usb_print("  ws-clear        blank all strips\r\n");
    usb_print("  N bri / bri?    master brightness cap 0..255\r\n");
    usb_print("  N npx / npx?    active pixels/strip (partial strips)\r\n");
    usb_print("  s i r g b ws-px / s r g b ws-fill / r g b ws-fill-all\r\n");
    usb_print("  s ws-show / ws-show-all / s ws-rand-fill\r\n");
    usb_print("Phase 2 (WiFi/HTTP):\r\n");
    usb_print("  prov-status     BLE/WiFi provisioning state + IP\r\n");
    usb_print("  prov-reset      clear WiFi creds, re-advertise\r\n");
    usb_print("  strip?          show HTTP-driven strip state + owner\r\n");
    usb_print("  strip-status    show BLE/WiFi indicator on the strip\r\n");
    usb_print("                  (orange flash=unconfigured, red strobe=failed,\r\n");
    usb_print("                   green breath=connected, white pulse=BLE write)\r\n");
    usb_print("  strip-engage / strip-release  HTTP vs Forth owns strip\r\n");
}

/* ( -- ) report provisioning + WiFi state */
static void w_prov_status(void) {
    const char *L[] = { "IDLE","CREDS_RECEIVED","COMMIT_REQUESTED",
                        "CONNECTING","CONNECTED","FAILED" };
    int st = s_prov_state;
    if (st < 0 || st >= 6) st = 0;
    char ssid[33], pass[65];
    bool has = craw_nvs_load_wifi_creds(ssid, pass);
    usb_printf("\r\nprov:  %s\r\n", L[st]);
    usb_printf("ble:   %s\r\n", craw_ble_provision_device_name());
    usb_printf("wifi:  %s\r\n", craw_wifi_is_connected() ? "connected" : "down");
    usb_printf("ssid:  %s\r\n", has ? ssid : "(none)");
    usb_printf("ip:    %s\r\n", s_ip_str);
}

/* ( -- ) clear WiFi creds and re-advertise */
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

/* ( -- ) print the HTTP-driven strip state + current owner */
static void w_strip_q(void) {
    strip_state_t s;
    strip_get_state(&s);
    const char *owner =
        (s_owner == OWNER_HTTP)   ? "HTTP"   :
        (s_owner == OWNER_FORTH)  ? "Forth"  :
                                    "status";
    usb_printf("\r\nstrip: %s  bri=%d%%  rgb=%d,%d,%d  pat=%s  spd=%d%%  owner=%s\r\n",
        s.on ? "ON" : "off", s.brightness_pct, s.r, s.g, s.b,
        STRIP_PATTERN_NAMES[s.pattern], s.pattern_speed_pct, owner);
}

/* ( -- ) hand the strip to the Forth REPL (render task stops touching it) */
static void w_strip_release(void) {
    s_owner = OWNER_FORTH;
    usb_print("\r\nowner=Forth (REPL self-test owns the strip)\r\n");
}

/* ( -- ) give the strip back to the HTTP-driven render task */
static void w_strip_engage(void) {
    s_owner = OWNER_HTTP;
    usb_print("\r\nowner=HTTP (render task drives the strip)\r\n");
}

/* ( -- ) show the provisioning-status pattern on the strip */
static void w_strip_status(void) {
    s_owner = OWNER_STATUS;
    usb_print("\r\nowner=status (BLE/WiFi indicator pattern)\r\n");
}

static void register_words(void) {
    forth_register_word("prov-status",  w_prov_status);
    forth_register_word("prov-reset",   w_prov_reset);
    forth_register_word("strip?",       w_strip_q);
    forth_register_word("strip-release",w_strip_release);
    forth_register_word("strip-engage", w_strip_engage);
    forth_register_word("strip-status", w_strip_status);
    forth_register_word("ws-px",        w_ws_px);
    forth_register_word("ws-fill",      w_ws_fill);
    forth_register_word("ws-fill-all",  w_ws_fill_all);
    forth_register_word("ws-rand-fill", w_ws_rand_fill);
    forth_register_word("ws-show",      w_ws_show);
    forth_register_word("ws-show-all",  w_ws_show_all);
    forth_register_word("ws-clear",     w_ws_clear);
    forth_register_word("bri",          w_bri);
    forth_register_word("bri?",         w_bri_q);
    forth_register_word("npx",          w_npx);
    forth_register_word("npx?",         w_npx_q);
    forth_register_word("rnd",          w_rnd);
    forth_register_word("nap",          w_nap);
    forth_register_word("strips",       w_strips);
}

/* High-level self-test + pattern words, defined in Forth and loaded at boot.
 * Editable live at the REPL — redefine any of these to tweak a test. */
static const char *FORTH_BUNDLE =
    "4 constant NSTRIP\n"
    /* one white pixel marching down a strip, clearing behind it */
    /* low-power bring-up: pixel 0 of each strip, very dim, one at a time.
     * Safe to run on USB power while diagnosing supply/brownout. */
    /* Patterns use full-scale colors; the `bri` cap is the single
     * brightness/current dial (every channel is scaled by it). */
    ": smoke ( -- )\n"
    "  NSTRIP 0 do\n"
    "    i 0 255 255 255 ws-px  i ws-show  400 nap\n"
    "    i 0 0 0 0 ws-px  i ws-show\n"
    "  loop\n"
    "  .\" smoke ok\" cr ;\n"
    /* light first 3 px of one strip green, to identify its wiring */
    ": id ( strip -- )\n"
    "  dup 0 0 255 0 ws-px  dup 1 0 255 0 ws-px\n"
    "  dup 2 0 255 0 ws-px  ws-show ;\n"
    ": walk1 ( strip -- )\n"
    "  dup 0 0 0 ws-fill\n"
    "  npx? 0 do\n"
    "    dup i 255 255 255 ws-px  dup ws-show\n"
    "    35 nap\n"
    "    dup i 0 0 0 ws-px\n"
    "  loop\n"
    "  dup ws-show drop ;\n"
    ": selftest-walk ( -- )\n"
    "  NSTRIP 0 do i walk1 loop\n"
    "  .\" walk done\" cr ;\n"
    ": selftest-allon ( -- )\n"
    "  255 255 255 ws-fill-all  ws-show-all\n"
    "  .\" all strips white -- every LED should be lit\" cr\n"
    "  3000 nap  ws-clear ;\n"
    ": selftest-bars ( -- )\n"
    "  255 0 0 ws-fill-all ws-show-all .\" RED\"   cr 1500 nap\n"
    "  0 255 0 ws-fill-all ws-show-all .\" GREEN\" cr 1500 nap\n"
    "  0 0 255 ws-fill-all ws-show-all .\" BLUE\"  cr 1500 nap\n"
    "  ws-clear  .\" bars done -- R/G/B must match labels\" cr ;\n"
    ": selftest ( -- )\n"
    "  .\" === SELF TEST ===\" cr\n"
    "  selftest-bars  selftest-allon  selftest-walk\n"
    "  .\" === SELF TEST COMPLETE ===\" cr ;\n"
    /* one frame of the ported campsynthesis random-color loop.
     * Strip 3 (orig strip4) keeps its ~2.4 s dwell before showing. */
    ": randframe ( -- )\n"
    "  0 ws-rand-fill 0 ws-show\n"
    "  1 ws-rand-fill 1 ws-show\n"
    "  2 ws-rand-fill 2 ws-show\n"
    "  3 ws-rand-fill 2400 nap 3 ws-show\n"
    "  1000 nap ;\n"
    ": randrun ( frames -- )  0 do randframe loop  ws-clear ;\n";

/* ---- Main ---- */

void app_main(void) {
    setup_usb_serial();
    vTaskDelay(pdMS_TO_TICKS(500));

    usb_print("\r\n\r\n");
    usb_print("============================================\r\n");
    usb_printf("  XIAO ESP32C3 IOT Lighting E4TH v%s\r\n", ESPIDFORTH_VERSION_STRING);
    usb_printf("  Build: %s %s\r\n", ESPIDFORTH_BUILD_DATE, ESPIDFORTH_BUILD_TIME);
    usb_print("  Grove WS2813 (60 px) via RMT + ESPIDFORTH\r\n");
    usb_print("============================================\r\n");

    ws_gpio_init();
    usb_print("Strip 0: Grove D0 (GPIO2 DIN + GPIO3 BIN, RMT). Blanked.\r\n");

    /* Forth FIRST: claim the dictionary heap while RAM is plentiful.
     * NimBLE + WiFi + httpd allocate large internal buffers; if they run
     * before forth_init the 64KB dictionary alloc comes up short and the
     * ESP32forth core (compiled from Forth at init: `.`, `words`, `do`...)
     * plus the bundle silently truncate — only C FFI words survive. */
    usb_printf("Free heap before Forth: %lu bytes\r\n",
        (unsigned long)esp_get_free_heap_size());
    if (forth_init(FORTH_HEAP_SIZE) != 0)
        usb_print("[ERR] forth_init failed (heap too small?)\r\n");
    register_words();
    if (forth_eval(FORTH_BUNDLE) != 0)
        usb_print("[warn] Forth bundle load reported an error\r\n");
    strip_ctl_init();
    usb_print("Forth engine + render task initialized.\r\n");

    /* Phase 2: NVS, then BLE FIRST so the controller gets its contiguous
     * ~40 KB DRAM block before WiFi grabs its share. (WiFi's allocations
     * are more tolerant of fragmentation than the NimBLE controller's.) */
    craw_nvs_init_flash();
    craw_nvs_migrate_wifi_profiles();

    craw_ble_provision_config_t prov_cfg = {
        .name_prefix = "MagNET-lighting",
        .role        = "lighting",
        /* BT SIG Appearance: 0x0580 = Generic Light Fixtures. Drives the
         * lightbulb icon in nRF Connect's scan list and after connect. */
        .appearance  = 0x0580,
    };
    craw_ble_provision_init(&prov_cfg, on_prov_event, NULL);
    /* (whether the radio is actually on the air is reported by the
     *  craw_ble_prov: advertising as '...' ESP_LOGI from ble_on_sync) */
    usb_printf("BLE init done; device name='%s'\r\n",
               craw_ble_provision_device_name());

    craw_wifi_init("MagNET-lighting", on_wifi_event, NULL);
    usb_printf("Free heap after BLE+WiFi: %lu bytes\r\n\r\n",
        (unsigned long)esp_get_free_heap_size());

    w_strips();

    /* Auto-connect if we already have stored WiFi creds */
    char ssid[33], pass[65];
    if (craw_nvs_load_wifi_creds(ssid, pass) && ssid[0]) {
        usb_printf("Stored WiFi '%s' — auto-connecting...\r\n", ssid);
        craw_ble_provision_set_status(CRAW_BLE_PROV_CONNECTING);
        s_prov_state = CRAW_BLE_PROV_CONNECTING;
        craw_wifi_connect(ssid, pass);
    } else {
        usb_print("No stored WiFi. Provision over BLE, then the UC2\r\n");
        usb_print("panel POSTs /api/v1/actuator/neopixel to drive the strip.\r\n");
    }
    usb_print("Strip shows BLE/WiFi status pattern until the first HTTP cmd.\r\n");
    usb_print("Use 'strip-release' for Forth self-test, 'strip-status' to\r\n");
    usb_print("force the indicator pattern back on.\r\n\r\n");

    forth_repl(uart_getchar, uart_putchar);
    forth_deinit();
}
