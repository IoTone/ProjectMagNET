#ifndef BOARD_STICKC_H
#define BOARD_STICKC_H

/* board_stickc — M5StickC Plus (original, AXP192) board layer for the Ear.
 *
 * Display (ST7789V2 via M5GFX), AXP192 battery telemetry, buttons, LED.
 * M5GFX auto-detects the board and owns I2C port 1 (SDA21/SCL22); the
 * AXP192 (0x34) is read through M5GFX's lgfx::i2c, so board_init() must
 * run before anything else touches that bus.
 *
 * Board knowledge harvested from M5StickCPlus-Blinky_Voice_XR — see
 * docs/PLAN.md §6. No full-screen sprite (64,800 B): everything draws
 * direct to the panel.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_BTN_A_GPIO   37   /* front, big — the thumb / push-to-talk */
#define BOARD_BTN_B_GPIO   39   /* side */
#define BOARD_LED_GPIO     10   /* red, active-low */

typedef struct {
    int  mv;          /* battery voltage, mV (-1 = read failed) */
    int  pct;         /* 0..100, approximate (-1 = unknown) */
    int  dischg_ma;   /* discharge current, mA */
    bool charging;
} board_batt_t;

typedef enum { BOARD_TONE_NORMAL, BOARD_TONE_OK, BOARD_TONE_WARN, BOARD_TONE_ALERT } board_tone_t;

typedef struct {
    const char *banner;     /* big status word, e.g. "NOT PAIRED" / passkey */
    board_tone_t tone;      /* banner colour */
    bool        banner_big; /* size-3 banner (passkey digits) */
    const char *line1;      /* small detail lines */
    const char *line2;
    const char *line3;
    /* Button legends, drawn next to the physical button (screen rotation 1,
     * USB on the right): B = top edge (bar along the top, arrows up),
     * A = front button right of the screen (bottom-right, arrow right).
     * NULL/"" = button does nothing in this state. */
    const char *btn_a;
    const char *btn_b;
    bool        live;       /* mic open: ear drawn red */
} board_screen_t;

void board_init(void);
void board_batt_read(board_batt_t *out);

/* Debounced button state (call board_buttons_poll() every ~10 ms). */
void board_buttons_poll(void);
bool board_btn_a_down(void);
bool board_btn_b_down(void);

/* Power key (bottom edge): 0 none, 1 short press, 2 long press (1.5 s).
 * Latched by the AXP192; reading clears it. */
int  board_power_key(void);
void board_power_off(void);           /* AXP192 shutdown; power key turns it back on */
int  board_axp_read(int reg);         /* diagnostics; -1 on I2C error */

void board_led(bool on);
void board_brightness(int pct);
void board_draw(const board_screen_t *s);

#ifdef __cplusplus
}
#endif
#endif
