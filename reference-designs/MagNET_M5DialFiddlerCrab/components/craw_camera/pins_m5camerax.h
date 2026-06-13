// M5Stack M5Camera X pin map (classic ESP32 + OV-series sensor, 8 MB QSPI PSRAM).
// Wiring is the CAMERA_MODEL_M5STACK_V2_PSRAM ("Model B") layout: SIOD=22 /
// VSYNC=25. This is confirmed against M5Stack's own firmware — see
// m5stack/M5Stack-Camera, face_qr/main/app_camera.cpp (the active M5Camera
// block): SIOD=22, SIOC=23, VSYNC=25, XCLK=27 @ 10 MHz, RESET=15, LED=14.
// (The earlier Model-A guess, SIOD=25/VSYNC=22, never ACKed on the SCCB bus,
//  which surfaces as esp_camera "Detected camera not supported" / 0x106.)
// The OV3660/OV2640 sensor is auto-detected by esp32-camera over SCCB.
#ifndef PINS_M5CAMERAX_H
#define PINS_M5CAMERAX_H

#define M5CAMERAX_PWDN_GPIO   -1   /* no power-down line on M5Camera */
#define M5CAMERAX_RESET_GPIO  15
#define M5CAMERAX_XCLK_GPIO   27
#define M5CAMERAX_SIOD_GPIO   22   /* I2C SDA to sensor (Model B) */
#define M5CAMERAX_SIOC_GPIO   23   /* I2C SCL to sensor */
#define M5CAMERAX_Y9_GPIO     19
#define M5CAMERAX_Y8_GPIO     36
#define M5CAMERAX_Y7_GPIO     18
#define M5CAMERAX_Y6_GPIO     39
#define M5CAMERAX_Y5_GPIO      5
#define M5CAMERAX_Y4_GPIO     34
#define M5CAMERAX_Y3_GPIO     35
#define M5CAMERAX_Y2_GPIO     32
#define M5CAMERAX_VSYNC_GPIO  25   /* (Model B) */
#define M5CAMERAX_HREF_GPIO   26
#define M5CAMERAX_PCLK_GPIO   21
#define M5CAMERAX_LED_GPIO    14   /* onboard LED (M5 CAMERA_LED_GPIO) */
#define M5CAMERAX_LED_INVERT   0

#endif
