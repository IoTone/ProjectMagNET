// M5Stack AtomS3R M12 pin map (ESP32-S3-PICO-1-N8R8, OV3660 sensor, M12 lens mount).
// Source: official M5Stack schematic (main_board_schematic.pdf) +
//         pin-map image (C126-M12_PinMap_01.jpg) +
//         docs.m5stack.com/en/core/AtomS3R-M12
// All GPIOs verified against all three sources — no discrepancies.
//
// POWER_N (GPIO 18): active-LOW enable for the shared internal 3.3V rail that
// feeds BOTH the OV3660 camera AND the BMI270/BMM150 IMU. Per M5Stack: "the
// internal I2C peripheral can only be powered on when GPIO18 is set low."
//   Drive LOW  → rail ON  → OV3660 powered
//   Drive HIGH → rail OFF
// ESP32-S3 GPIOs boot as inputs, leaving the rail off. craw_camera.c drives
// this LOW (then waits) before esp_camera_init via s_power_gpio. Do NOT use as
// pin_pwdn — esp32-camera's pwdn is active-HIGH (opposite polarity).
//
// RESET (GPIO 38): the OV3660 hardware reset is driven MANUALLY in firmware
// (M5Stack's own driver leaves the esp32-camera pin_reset = -1 and pulses GPIO38
// itself, because the sensor needs a ~100 ms reset-low + ~1 s settle that the
// driver's built-in 10 ms pulse is too short to provide). craw_camera.c handles
// this via s_manual_reset_gpio; pin_reset stays -1 so the driver doesn't also
// pulse it with the wrong (too-short) timing. Source: m5stack/M5AtomS3
// examples/Basics/camera + M5Stack HA integration docs + community configs.
//
// No separate RESET or PWDN pin. No flash LED on the camera module.
// IR LED is on GPIO 47 (driven via FET, not a simple GPIO-level flash).
// Status LED (green) is on GPIO 0 — also not suitable for camera flash.
#ifndef PINS_ATOMS3R_H
#define PINS_ATOMS3R_H

#define ATOMS3R_PWDN_GPIO   -1   /* no pwdn — power controlled via ATOMS3R_POWER_GPIO */
#define ATOMS3R_RESET_GPIO  -1   /* driver-level reset OFF — done manually, see ATOMS3R_RESET_N_GPIO */
#define ATOMS3R_XCLK_GPIO   21
#define ATOMS3R_SIOD_GPIO   12   /* SCCB SDA — verified from schematic */
#define ATOMS3R_SIOC_GPIO    9   /* SCCB SCL — verified from schematic */
#define ATOMS3R_Y9_GPIO     13
#define ATOMS3R_Y8_GPIO     11
#define ATOMS3R_Y7_GPIO     17
#define ATOMS3R_Y6_GPIO      4
#define ATOMS3R_Y5_GPIO     48
#define ATOMS3R_Y4_GPIO     46
#define ATOMS3R_Y3_GPIO     42
#define ATOMS3R_Y2_GPIO      3
#define ATOMS3R_VSYNC_GPIO  10
#define ATOMS3R_HREF_GPIO   14
#define ATOMS3R_PCLK_GPIO   40
#define ATOMS3R_LED_GPIO    -1   /* no camera flash LED */
#define ATOMS3R_LED_INVERT   0

/* Active-LOW power enable (POWER_N). Driven LOW before esp_camera_init. */
#define ATOMS3R_POWER_GPIO  18

/* OV3660 hardware reset, pulsed manually with M5Stack's timing (reset-low
 * ~100 ms, settle ~1 s). Kept separate from ATOMS3R_RESET_GPIO so the driver
 * does not also pulse it with its too-short built-in 10 ms timing. */
#define ATOMS3R_RESET_N_GPIO  38

#endif
