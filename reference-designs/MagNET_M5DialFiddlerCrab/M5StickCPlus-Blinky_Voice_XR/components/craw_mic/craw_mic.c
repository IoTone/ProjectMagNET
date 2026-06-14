/*
 * craw_mic — SPM1423 PDM mic via I2S PDM-RX (ESP-IDF new I2S driver).
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include "craw_mic.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"

static const char *TAG = "craw_mic";

static i2s_chan_handle_t s_rx     = NULL;
static int               s_rate   = CRAW_MIC_SAMPLE_RATE;
static bool              s_inited  = false;

esp_err_t craw_mic_init(const craw_mic_config_t *cfg) {
    if (s_inited) return ESP_OK;

    int clk = (cfg && cfg->clk_gpio) ? cfg->clk_gpio : CRAW_MIC_CLK_GPIO;
    int din = (cfg && cfg->din_gpio) ? cfg->din_gpio : CRAW_MIC_DIN_GPIO;
    s_rate  = (cfg && cfg->sample_rate) ? cfg->sample_rate : CRAW_MIC_SAMPLE_RATE;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: 0x%x", err);
        return err;
    }

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(s_rate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                   I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = (gpio_num_t)clk,
            .din = (gpio_num_t)din,
            .invert_flags = { .clk_inv = false },
        },
    };
    err = i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init_pdm_rx_mode failed: 0x%x (clk=%d din=%d)", err, clk, din);
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "PDM mic ready: clk=%d din=%d rate=%d Hz (%u B/s)",
             clk, din, s_rate, (unsigned)craw_mic_bytes_per_sec());
    return ESP_OK;
}

esp_err_t craw_mic_deinit(void) {
    if (!s_inited) return ESP_OK;
    i2s_del_channel(s_rx);
    s_rx = NULL;
    s_inited = false;
    return ESP_OK;
}

bool   craw_mic_is_initialized(void) { return s_inited; }
int    craw_mic_sample_rate(void)    { return s_rate; }
size_t craw_mic_bytes_per_sec(void)  { return (size_t)s_rate * sizeof(int16_t); }

int craw_mic_max_record_seconds(size_t budget_bytes) {
    size_t bps = craw_mic_bytes_per_sec();
    if (bps == 0) return 0;
    return (int)(budget_bytes / bps);
}

esp_err_t craw_mic_record(int16_t *buf, size_t max_samples, int seconds,
                          size_t *out_samples,
                          craw_mic_progress_cb_t progress_cb, void *cb_ctx) {
    if (!s_inited || !buf || max_samples == 0) return ESP_ERR_INVALID_STATE;
    if (out_samples) *out_samples = 0;

    size_t want = (size_t)seconds * (size_t)s_rate;
    if (want > max_samples) want = max_samples;   /* clamp to buffer */

    esp_err_t err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel_enable failed: 0x%x", err);
        return err;
    }

    /* The first PDM block after enable is often a DC/settling transient;
     * discard ~30 ms so the meter and recording start clean. */
    {
        int16_t scratch[256];
        size_t got = 0;
        i2s_channel_read(s_rx, scratch, sizeof(scratch), &got, pdMS_TO_TICKS(50));
    }

    size_t total = 0;
    while (total < want) {
        size_t chunk_bytes = (want - total) * sizeof(int16_t);
        size_t got = 0;
        err = i2s_channel_read(s_rx, buf + total, chunk_bytes, &got,
                               pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "channel_read: 0x%x (got %u)", err, (unsigned)got);
            break;
        }
        total += got / sizeof(int16_t);
        if (progress_cb) progress_cb((float)total / (float)want, cb_ctx);
    }

    i2s_channel_disable(s_rx);
    if (out_samples) *out_samples = total;
    ESP_LOGI(TAG, "recorded %u samples (%.2f s)", (unsigned)total,
             (float)total / (float)s_rate);
    return ESP_OK;
}

int craw_mic_peak(const int16_t *buf, size_t n) {
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        int v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > peak) peak = v;
    }
    return peak;
}
