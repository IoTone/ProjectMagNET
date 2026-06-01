#include "craw_status_led.h"

#include <math.h>
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "craw_led";

static led_strip_handle_t  s_strip = NULL;
static craw_led_mode_t     s_mode = CRAW_LED_OFF;
static int                 s_data = 0;
static int                 s_phase_ms = 0;

/* HSV (h ∈ [0, 360), s/v ∈ [0,1]) → RGB ∈ [0,255]. */
static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (s <= 0.0f) {
        uint8_t k = (uint8_t)(v * 255.0f);
        *r = *g = *b = k;
        return;
    }
    while (h >= 360.0f) h -= 360.0f;
    while (h < 0.0f) h += 360.0f;
    int i = (int)(h / 60.0f);
    float f = (h / 60.0f) - (float)i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    float fr, fg, fb;
    switch (i % 6) {
        case 0:  fr = v; fg = t; fb = p; break;
        case 1:  fr = q; fg = v; fb = p; break;
        case 2:  fr = p; fg = v; fb = t; break;
        case 3:  fr = p; fg = q; fb = v; break;
        case 4:  fr = t; fg = p; fb = v; break;
        default: fr = v; fg = p; fb = q; break;
    }
    *r = (uint8_t)(fr * 255.0f);
    *g = (uint8_t)(fg * 255.0f);
    *b = (uint8_t)(fb * 255.0f);
}

/* 0..1 sine envelope from a phase fraction. */
static float pulse(float frac) {
    return 0.5f + 0.5f * sinf(frac * 2.0f * (float)M_PI);
}

esp_err_t craw_status_led_init(int gpio) {
    led_strip_config_t cfg = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed: %s", esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);
    s_mode = CRAW_LED_BOOTING;
    s_data = 0;
    s_phase_ms = 0;
    return ESP_OK;
}

void craw_status_led_set_mode(craw_led_mode_t mode, int data_int) {
    if (mode != s_mode) s_phase_ms = 0;
    s_mode = mode;
    s_data = data_int;
}

void craw_status_led_tick(int dt_ms) {
    if (!s_strip) return;
    s_phase_ms += dt_ms;

    uint8_t r = 0, g = 0, b = 0;

    switch (s_mode) {
        case CRAW_LED_OFF:
            break;

        case CRAW_LED_BOOTING:
            /* Solid warm amber — distinctive, not bright. */
            r = 200; g = 80; b = 0;
            break;

        case CRAW_LED_IDLE: {
            /* Yellow gentle breath — radar is up but nothing in the cone. */
            float frac = (float)(s_phase_ms % 4000) / 4000.0f;
            float v = 0.25f + 0.25f * pulse(frac);
            hsv_to_rgb(48.0f, 0.95f, v, &r, &g, &b);
            (void)s_data;
            break;
        }

        case CRAW_LED_PRESENCE: {
            /* Blue pulse — radar is actively tracking a person. The
             * `data_int` slot is reserved for future use (e.g. BPM-tinted
             * shade); for now the colour is single-tone for clarity. */
            float frac = (float)(s_phase_ms % 2000) / 2000.0f;
            float v = 0.40f + 0.30f * pulse(frac);
            hsv_to_rgb(220.0f, 0.95f, v, &r, &g, &b);
            (void)s_data;
            break;
        }

        case CRAW_LED_ERROR:
            /* 1 Hz red blink. */
            if ((s_phase_ms / 500) % 2 == 0) { r = 200; g = 0; b = 0; }
            break;

        case CRAW_LED_TEST_OK:
            /* Solid green — radar self-test passed. */
            r = 0; g = 200; b = 0;
            break;

        case CRAW_LED_TEST_FAIL:
            /* 4 Hz red flash — self-test failed. */
            if ((s_phase_ms / 125) % 2 == 0) { r = 220; g = 0; b = 0; }
            break;

        case CRAW_LED_WIFI_OFFLINE:
            /* 4 Hz yellow blink — WiFi disconnected or all attempts failed.
             * Same cadence as TEST_FAIL but yellow (hue 48°) at full brightness
             * so the eye reads it as "alarm" not "background breath" the way
             * the slow IDLE yellow does. Distinct from TEST_FAIL by colour. */
            if ((s_phase_ms / 125) % 2 == 0) {
                hsv_to_rgb(48.0f, 1.0f, 0.85f, &r, &g, &b);
            }
            break;
    }

    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}
