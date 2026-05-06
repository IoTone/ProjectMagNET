/*
 * craw_status_led — semantic wrapper around the kit's WS2812 status LED.
 *
 * The kit has one addressable RGB LED. Rather than drive raw RGB everywhere,
 * the rest of the firmware sets a semantic mode (booting / idle / presence /
 * error / off) and lets this component handle colour and animation.
 *
 * Animation is driven by a `tick(dt_ms)` call from the main loop — no
 * dedicated task. Call ~20 Hz for smooth pulsing.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CRAW_LED_OFF = 0,
    CRAW_LED_BOOTING,        /* solid amber */
    CRAW_LED_IDLE,           /* slow soft cyan pulse, ~4s period */
    CRAW_LED_PRESENCE,       /* pulse, hue mapped from BPM (50→blue-violet, 100→red) */
    CRAW_LED_ERROR,          /* 1 Hz red blink */
    CRAW_LED_TEST_OK,        /* solid green — self-test passed */
    CRAW_LED_TEST_FAIL,      /* 4 Hz red flash — self-test failed */
} craw_led_mode_t;

/** Initialize the WS2812 driver on the given GPIO. One LED. */
esp_err_t craw_status_led_init(int gpio);

/**
 * Set the active mode.
 * `data_int` is mode-specific:
 *   - PRESENCE: heart rate in BPM (drives hue)
 *   - other modes: ignored
 */
void craw_status_led_set_mode(craw_led_mode_t mode, int data_int);

/** Advance the animation by dt_ms and refresh the LED. Call every ~50 ms. */
void craw_status_led_tick(int dt_ms);

#ifdef __cplusplus
}
#endif
