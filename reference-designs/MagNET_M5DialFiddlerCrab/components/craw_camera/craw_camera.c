/*
 * craw_camera — pin-map + esp32-camera wrapper.
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include "craw_camera.h"
#include "pins_ai_thinker.h"
#include "pins_m5cams3.h"

#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "craw_camera";

static craw_camera_board_t s_board     = CRAW_CAMERA_BOARD_AI_THINKER;
static int                 s_led_gpio  = -1;
static bool                s_led_invert = false;
static bool                s_initialized = false;

static void build_cfg(const craw_camera_config_t *in, camera_config_t *out) {
    memset(out, 0, sizeof(*out));

    switch (in->board) {
    case CRAW_CAMERA_BOARD_AI_THINKER:
        out->pin_pwdn     = AI_THINKER_PWDN_GPIO;
        out->pin_reset    = AI_THINKER_RESET_GPIO;
        out->pin_xclk     = AI_THINKER_XCLK_GPIO;
        out->pin_sccb_sda = AI_THINKER_SIOD_GPIO;
        out->pin_sccb_scl = AI_THINKER_SIOC_GPIO;
        out->pin_d7       = AI_THINKER_Y9_GPIO;
        out->pin_d6       = AI_THINKER_Y8_GPIO;
        out->pin_d5       = AI_THINKER_Y7_GPIO;
        out->pin_d4       = AI_THINKER_Y6_GPIO;
        out->pin_d3       = AI_THINKER_Y5_GPIO;
        out->pin_d2       = AI_THINKER_Y4_GPIO;
        out->pin_d1       = AI_THINKER_Y3_GPIO;
        out->pin_d0       = AI_THINKER_Y2_GPIO;
        out->pin_vsync    = AI_THINKER_VSYNC_GPIO;
        out->pin_href     = AI_THINKER_HREF_GPIO;
        out->pin_pclk     = AI_THINKER_PCLK_GPIO;
        s_led_gpio        = AI_THINKER_LED_GPIO;
        s_led_invert      = AI_THINKER_LED_INVERT;
        break;

    case CRAW_CAMERA_BOARD_M5CAMS3:
        out->pin_pwdn     = M5CAMS3_PWDN_GPIO;
        out->pin_reset    = M5CAMS3_RESET_GPIO;
        out->pin_xclk     = M5CAMS3_XCLK_GPIO;
        out->pin_sccb_sda = M5CAMS3_SIOD_GPIO;
        out->pin_sccb_scl = M5CAMS3_SIOC_GPIO;
        out->pin_d7       = M5CAMS3_Y9_GPIO;
        out->pin_d6       = M5CAMS3_Y8_GPIO;
        out->pin_d5       = M5CAMS3_Y7_GPIO;
        out->pin_d4       = M5CAMS3_Y6_GPIO;
        out->pin_d3       = M5CAMS3_Y5_GPIO;
        out->pin_d2       = M5CAMS3_Y4_GPIO;
        out->pin_d1       = M5CAMS3_Y3_GPIO;
        out->pin_d0       = M5CAMS3_Y2_GPIO;
        out->pin_vsync    = M5CAMS3_VSYNC_GPIO;
        out->pin_href     = M5CAMS3_HREF_GPIO;
        out->pin_pclk     = M5CAMS3_PCLK_GPIO;
        s_led_gpio        = M5CAMS3_LED_GPIO;
        s_led_invert      = M5CAMS3_LED_INVERT;
        break;
    }

    out->xclk_freq_hz = in->xclk_freq_hz ? in->xclk_freq_hz : 20000000;
    out->ledc_timer   = LEDC_TIMER_0;
    out->ledc_channel = LEDC_CHANNEL_0;

    out->pixel_format = in->pixel_format ? in->pixel_format : PIXFORMAT_JPEG;
    out->frame_size   = in->frame_size   ? (framesize_t)in->frame_size : FRAMESIZE_SVGA;
    out->jpeg_quality = in->jpeg_quality ? in->jpeg_quality : 12;
    out->fb_count     = in->fb_count     ? in->fb_count     : 2;
    out->fb_location  = CAMERA_FB_IN_PSRAM;
    out->grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
}

static void init_flash_gpio(void) {
    if (s_led_gpio < 0) return;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_led_gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level((gpio_num_t)s_led_gpio, s_led_invert ? 1 : 0);
}

int craw_camera_init(const craw_camera_config_t *cfg) {
    if (s_initialized) return 0;
    if (!cfg) return -1;
    s_board = cfg->board;

    camera_config_t c;
    build_cfg(cfg, &c);

    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", err);
        return (int)err;
    }

    init_flash_gpio();
    s_initialized = true;
    ESP_LOGI(TAG, "init ok: board=%s framesize=%d quality=%d fb=%d",
             craw_camera_board_name(), c.frame_size, c.jpeg_quality, c.fb_count);
    return 0;
}

int craw_camera_deinit(void) {
    if (!s_initialized) return 0;
    esp_camera_deinit();
    s_initialized = false;
    return 0;
}

camera_fb_t *craw_camera_capture(void) {
    if (!s_initialized) return NULL;
    return esp_camera_fb_get();
}

void craw_camera_release(camera_fb_t *fb) {
    if (fb) esp_camera_fb_return(fb);
}

int craw_camera_set_quality(int q) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_quality) return -1;
    if (q < 0)  q = 0;
    if (q > 63) q = 63;
    return s->set_quality(s, q);
}

int craw_camera_set_framesize(int fs) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_framesize) return -1;
    return s->set_framesize(s, (framesize_t)fs);
}

int craw_camera_set_vflip(int on) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_vflip) return -1;
    return s->set_vflip(s, on ? 1 : 0);
}

int craw_camera_set_hmirror(int on) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_hmirror) return -1;
    return s->set_hmirror(s, on ? 1 : 0);
}

void craw_camera_flash_on(void) {
    if (s_led_gpio < 0) return;
    gpio_set_level((gpio_num_t)s_led_gpio, s_led_invert ? 0 : 1);
}

void craw_camera_flash_off(void) {
    if (s_led_gpio < 0) return;
    gpio_set_level((gpio_num_t)s_led_gpio, s_led_invert ? 1 : 0);
}

const char *craw_camera_board_name(void) {
    switch (s_board) {
    case CRAW_CAMERA_BOARD_AI_THINKER: return "AI-Thinker ESP32-CAM";
    case CRAW_CAMERA_BOARD_M5CAMS3:    return "M5Stack Unit CamS3";
    default:                           return "unknown";
    }
}

int craw_camera_flash_gpio(void) {
    return s_led_gpio;
}
