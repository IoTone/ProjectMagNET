/*
 * Single WS2812-family RGB LED on GPIO8, driven with the RMT TX peripheral.
 *
 * Written against ESP-IDF's RMT bytes-encoder rather than pulling in the
 * espressif/led_strip managed component: that would need a fetch from the
 * component registry, and this is ~40 lines. The C6 has 2 RMT TX channels.
 *
 * The LED is the attention-getting surface on this board — NOT the backlight,
 * which is thermally capped (see UI_BL_MAX).
 */
#include "magnet_ui.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#define PIN_LED   8
#define RMT_HZ    (10 * 1000 * 1000)   /* 0.1 us per tick */

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;

esp_err_t led_init(void) {
    rmt_tx_channel_config_t ch = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = PIN_LED,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&ch, &s_chan);
    if (err != ESP_OK) return err;

    /* WS2812 bit timing at 0.1 us/tick:
     *   0 bit = 0.3 us high, 0.9 us low
     *   1 bit = 0.9 us high, 0.3 us low
     * MSB first, which is what the part expects. */
    rmt_bytes_encoder_config_t bytes = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&bytes, &s_enc);
    if (err != ESP_OK) return err;

    return rmt_enable(s_chan);
}

void led_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_chan) return;
    /* WS2812 wire order is GRB, not RGB — the single most common way to get a
     * green LED when you asked for red. */
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_chan, s_enc, grb, sizeof(grb), &tx);
    rmt_tx_wait_all_done(s_chan, 100);
}
