/* magnet_led.c — see magnet_led.h. RGB path lifted from the proven RMT
 * bytes-encoder driver in MagNET_OTA_WaveC6LED (deliberately no
 * espressif/led_strip managed component: registry fetches are how the
 * bmi270/IDF-5.4 trap got in, and this is ~40 lines). */
#include "magnet_led.h"

#if MN_ENABLE_LED

#include "driver/gpio.h"

#if defined(MN_LED_RGB_GPIO)

#include "driver/rmt_tx.h"

#define RMT_HZ (10 * 1000 * 1000)      /* 0.1 us per tick */

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;

int mn_led_init(void) {
#ifdef MN_LED_EN_GPIO
    /* LED power rail (NanoC6 G19): without this the pixel never lights,
     * which reads as a wrong-data-pin bug. Enable BEFORE the first frame. */
    gpio_reset_pin((gpio_num_t)MN_LED_EN_GPIO);
    gpio_set_direction((gpio_num_t)MN_LED_EN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)MN_LED_EN_GPIO, 1);
#endif
    gpio_reset_pin((gpio_num_t)MN_LED_RGB_GPIO);
    rmt_tx_channel_config_t ch = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = MN_LED_RGB_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&ch, &s_chan);
    if (err != ESP_OK) { s_chan = NULL; return (int)err; }
    /* WS2812 bit timing at 0.1 us/tick: 0 = 0.3us hi / 0.9us lo,
     * 1 = 0.9us hi / 0.3us lo, MSB first. */
    rmt_bytes_encoder_config_t bytes = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&bytes, &s_enc);
    if (err == ESP_OK) err = rmt_enable(s_chan);
    if (err != ESP_OK) { s_chan = NULL; return (int)err; }
    mn_led_set(0, 0, 0);               /* known state (part may power up lit) */
    return 0;
}

int mn_led_ok(void) { return s_chan != NULL; }

static int s_last_err;                 /* last transmit rc, for diagnosis */
int mn_led_last(void) { return s_last_err; }

void mn_led_set(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_chan) return;
    /* WS2812 wire order is GRB, not RGB — the single most common way to get
     * a green LED when you asked for red. */
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_chan, s_enc, grb, sizeof(grb), &tx);
    if (err == ESP_OK) err = rmt_tx_wait_all_done(s_chan, 100);
    s_last_err = (int)err;
}

#elif defined(MN_LED_GPIO)

#ifndef MN_LED_ACTIVE_LOW
#define MN_LED_ACTIVE_LOW 0
#endif

int mn_led_init(void) {
    gpio_reset_pin((gpio_num_t)MN_LED_GPIO);
    gpio_set_direction((gpio_num_t)MN_LED_GPIO, GPIO_MODE_OUTPUT);
    mn_led_set(0, 0, 0);
    return 0;
}

int mn_led_ok(void)   { return 1; }
int mn_led_last(void) { return 0; }

void mn_led_set(uint8_t r, uint8_t g, uint8_t b) {
    int on = (r || g || b) ? 1 : 0;
    gpio_set_level((gpio_num_t)MN_LED_GPIO, MN_LED_ACTIVE_LOW ? !on : on);
}

#else
#error "MN_ENABLE_LED=1 needs MN_LED_RGB_GPIO or MN_LED_GPIO"
#endif

#endif /* MN_ENABLE_LED */
