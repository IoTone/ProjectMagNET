// AI-Thinker ESP32-CAM pin map (classic ESP32 + OV2640). Canonical reference:
// https://github.com/espressif/arduino-esp32/.../camera_pins.h
#ifndef PINS_AI_THINKER_H
#define PINS_AI_THINKER_H

#define AI_THINKER_PWDN_GPIO   32
#define AI_THINKER_RESET_GPIO  -1   /* tied to reset circuit */
#define AI_THINKER_XCLK_GPIO    0   /* also boot strap — fine, only sampled at reset */
#define AI_THINKER_SIOD_GPIO   26   /* I2C SDA to sensor */
#define AI_THINKER_SIOC_GPIO   27   /* I2C SCL to sensor */
#define AI_THINKER_Y9_GPIO     35
#define AI_THINKER_Y8_GPIO     34
#define AI_THINKER_Y7_GPIO     39
#define AI_THINKER_Y6_GPIO     36
#define AI_THINKER_Y5_GPIO     21
#define AI_THINKER_Y4_GPIO     19
#define AI_THINKER_Y3_GPIO     18
#define AI_THINKER_Y2_GPIO      5
#define AI_THINKER_VSYNC_GPIO  25
#define AI_THINKER_HREF_GPIO   23
#define AI_THINKER_PCLK_GPIO   22
#define AI_THINKER_LED_GPIO     4   /* bright white flash LED, active high */
#define AI_THINKER_LED_INVERT   0

#endif
