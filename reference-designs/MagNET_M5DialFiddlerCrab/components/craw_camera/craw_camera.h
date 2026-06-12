#ifndef CRAW_CAMERA_H
#define CRAW_CAMERA_H
#define CRAW_CAMERA_VERSION "0.1.0"

// craw_camera — thin wrapper over espressif/esp32-camera for MagNET hive
// camera nodes. Owns a per-board pin map so the rest of the project is
// chip-neutral. Preserves the stock esp32-camera API shape (camera_fb_t,
// sensor controls) so existing examples translate verbatim.
//
// Usage:
//   craw_camera_config_t cfg = {
//       .board        = CRAW_CAMERA_BOARD_AI_THINKER,  // or _M5CAMS3
//       .xclk_freq_hz = 20000000,
//       .jpeg_quality = 12,        // lower = better
//       .frame_size   = FRAMESIZE_SVGA,
//       .fb_count     = 2,
//   };
//   craw_camera_init(&cfg);
//   camera_fb_t *fb = craw_camera_capture();
//   ...write fb->buf, fb->len somewhere...
//   craw_camera_release(fb);

#include <stdbool.h>
#include <stdint.h>
#include "esp_camera.h"   // camera_fb_t, framesize_t, pixformat_t, etc.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CRAW_CAMERA_BOARD_AI_THINKER = 0,  // ESP32 + OV2640 (AI-Thinker ESP32-CAM)
    CRAW_CAMERA_BOARD_M5CAMS3    = 1,  // ESP32-S3 + OV2640 (M5Stack Unit CamS3)
    CRAW_CAMERA_BOARD_M5CAMERAX  = 2,  // ESP32 + OV3660 (M5Stack M5Camera X)
    // Add new boards here. Each needs a pins_<name>.h in this component.
} craw_camera_board_t;

typedef struct {
    craw_camera_board_t board;
    int          xclk_freq_hz;   // 0 → default 20 MHz
    int          jpeg_quality;   // 0..63, lower = better quality; 0 → default 12
    int          frame_size;     // framesize_t; 0 → default FRAMESIZE_SVGA
    int          fb_count;       // 0 → default 2
    pixformat_t  pixel_format;   // 0 → default PIXFORMAT_JPEG
} craw_camera_config_t;

// Initialize the sensor and allocate frame buffers in PSRAM. Returns ESP_OK
// on success, or an esp_err_t from esp_camera_init() on failure. PSRAM must
// be enabled in sdkconfig.
int craw_camera_init(const craw_camera_config_t *cfg);

// Tear down. Rarely needed.
int craw_camera_deinit(void);

// Acquire one frame. Caller MUST call craw_camera_release() when done with
// fb->buf. Returns NULL on failure (e.g. DMA underrun). Blocking; typical
// capture time is 30–100 ms depending on frame size.
camera_fb_t *craw_camera_capture(void);

// Release a frame buffer back to the driver for reuse.
void craw_camera_release(camera_fb_t *fb);

// Runtime sensor controls. Wrap sensor_t *s = esp_camera_sensor_get() and
// call s->set_quality / s->set_framesize. Return 0 on success.
/* Sensor mutators. Every one of these also writes the updated value to NVS
 * (namespace "craw_camera"), so runtime changes survive reboot. If NVS
 * write fails, the sensor change still applies — we only log the miss. */
int craw_camera_set_quality(int q);
int craw_camera_set_framesize(int fs);
int craw_camera_set_vflip(int on);    /* 0/1 — vertical flip (upside-down) */
int craw_camera_set_hmirror(int on);  /* 0/1 — horizontal mirror */

/* Read NVS-persisted settings (if any) and apply them to the current
 * sensor. Call once after craw_camera_init. No-op if NVS has no entries. */
void craw_camera_apply_saved_settings(void);

/* Clear persisted settings (revert to firmware defaults on next boot). */
void craw_camera_clear_saved_settings(void);

/* Persist an XCLK override (in MHz, valid 4..24). Takes effect on NEXT
 * boot — XCLK is locked at esp_camera_init time and can't be changed
 * live. Dropping from 20 → 10 often fixes NO-SOI / frame-timeout errors
 * on AI-Thinker ESP32-CAM when the 3.3 V rail dips under WiFi + camera
 * load. Value is stored in NVS under key "xclk". */
void craw_camera_set_xclk_mhz(int mhz);

// Flash / status LED helpers. No-op if the board has no LED_GPIO_NUM.
void craw_camera_flash_on(void);
void craw_camera_flash_off(void);

// Getters.
const char *craw_camera_board_name(void);
int         craw_camera_flash_gpio(void);   // -1 if none

#ifdef __cplusplus
}
#endif
#endif
