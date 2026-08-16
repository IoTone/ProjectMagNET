/* magnet_led.h — on-board LED as a mesh-visible surface (lighting phase 1).
 *
 * Compiled out entirely unless MN_ENABLE_LED=1 (env esp32c6_ble_led), same
 * discipline as magnet_bot.h: hardware-validated builds stay bit-identical.
 * Two board shapes, chosen by pin flags:
 *
 *   -DMN_LED_RGB_GPIO=<n>          WS2812-family addressable LED via RMT
 *   -DMN_LED_EN_GPIO=<n>           (optional) LED power rail, driven high
 *                                  (M5NanoC6: data G20, enable G19)
 *   -DMN_LED_GPIO=<n>              plain single-colour LED, on/off only
 *   -DMN_LED_ACTIVE_LOW=1          (XIAO ESP32C6: G15, active-low)
 *
 * mn_led_set(0,0,0) is "off" on both shapes; a plain LED lights when any
 * channel is nonzero. Host side: `LED <r> <g> <b> | OFF`; Forth: `led!`.
 */
#ifndef MAGNET_LED_H
#define MAGNET_LED_H

#include <stdint.h>

#ifndef MN_ENABLE_LED
#define MN_ENABLE_LED 0
#endif

#if MN_ENABLE_LED

int  mn_led_init(void);                          /* 0 on success, else esp_err */
int  mn_led_ok(void);                            /* 1 once init succeeded */
int  mn_led_last(void);                          /* last transmit rc (0 = ok) */
void mn_led_set(uint8_t r, uint8_t g, uint8_t b);
/* colour + rhythm: period_ms 0 = solid, else 50%-duty strobe. The bench
 * NanoC6 case is blue-tinted translucent plastic — hue alone is ambiguous
 * through it, so meaning rides on the blink rate first, colour second. */
void mn_led_pattern(uint8_t r, uint8_t g, uint8_t b, uint32_t period_ms);

#else

static inline int  mn_led_init(void) { return -1; }
static inline int  mn_led_ok(void)   { return 0; }
static inline int  mn_led_last(void) { return 0; }
static inline void mn_led_set(uint8_t r, uint8_t g, uint8_t b)
    { (void)r; (void)g; (void)b; }
static inline void mn_led_pattern(uint8_t r, uint8_t g, uint8_t b, uint32_t p)
    { (void)r; (void)g; (void)b; (void)p; }

#endif /* MN_ENABLE_LED */
#endif /* MAGNET_LED_H */
