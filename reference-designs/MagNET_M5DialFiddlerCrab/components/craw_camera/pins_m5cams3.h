// M5Stack Unit CamS3 pin map (ESP32-S3 + OV2640).
// Reference: https://docs.m5stack.com/en/unit/Unit-CamS3
// If you bring this up on hardware and the sensor enum fails, verify these
// against the schematic on your unit revision — M5Stack has shipped minor
// variants.
#ifndef PINS_M5CAMS3_H
#define PINS_M5CAMS3_H

#define M5CAMS3_PWDN_GPIO   -1
#define M5CAMS3_RESET_GPIO  21
#define M5CAMS3_XCLK_GPIO   11
#define M5CAMS3_SIOD_GPIO   17    /* I2C SDA */
#define M5CAMS3_SIOC_GPIO   41    /* I2C SCL */
#define M5CAMS3_Y9_GPIO     13
#define M5CAMS3_Y8_GPIO      4
#define M5CAMS3_Y7_GPIO     10
#define M5CAMS3_Y6_GPIO      5
#define M5CAMS3_Y5_GPIO      7
#define M5CAMS3_Y4_GPIO     16
#define M5CAMS3_Y3_GPIO     15
#define M5CAMS3_Y2_GPIO      6
#define M5CAMS3_VSYNC_GPIO  42
#define M5CAMS3_HREF_GPIO   18
#define M5CAMS3_PCLK_GPIO   12
#define M5CAMS3_LED_GPIO    14    /* small status LED */
#define M5CAMS3_LED_INVERT   0

#endif
