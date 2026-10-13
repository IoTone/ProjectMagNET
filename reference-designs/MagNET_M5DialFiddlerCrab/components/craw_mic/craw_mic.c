/*
 * craw_mic — SPM1423 PDM mic via I2S PDM-RX (ESP-IDF new I2S driver).
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include "craw_mic.h"

#include <string.h>
#include <math.h>
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
static bool              s_streaming = false;

/* Discard the PDM start-up transient after channel enable. Measured on a
 * StickC Plus (2026-10-06): output starts at -32768 and needs ~5 blocks
 * (80 ms at 16 kHz) to reach its DC level; the old 16 ms discard left the
 * transient in every one-shot recording (peak pinned at 32767). */
#define SETTLE_BLOCKS 6      /* x 256 samples = 96 ms at 16 kHz */
static void settle(void) {
    int16_t scratch[256];
    for (int b = 0; b < SETTLE_BLOCKS; b++) {
        size_t got = 0;
        i2s_channel_read(s_rx, scratch, sizeof(scratch), &got, pdMS_TO_TICKS(100));
        int mn = 32767, mx = -32768;
        for (size_t i = 0; i < got / 2; i++) {
            if (scratch[i] < mn) mn = scratch[i];
            if (scratch[i] > mx) mx = scratch[i];
        }
        ESP_LOGD(TAG, "settle blk %d: min %d max %d", b, mn, mx);
    }
}

esp_err_t craw_mic_init(const craw_mic_config_t *cfg) {
    if (s_inited) return ESP_OK;

    int clk = (cfg && cfg->clk_gpio) ? cfg->clk_gpio : CRAW_MIC_CLK_GPIO;
    int din = (cfg && cfg->din_gpio) ? cfg->din_gpio : CRAW_MIC_DIN_GPIO;
    s_rate  = (cfg && cfg->sample_rate) ? cfg->sample_rate : CRAW_MIC_SAMPLE_RATE;
    bool left = cfg && cfg->slot_left;

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
    pdm_cfg.slot_cfg.slot_mask = left ? I2S_PDM_SLOT_LEFT : I2S_PDM_SLOT_RIGHT;
    /* x128 oversampling (PDM clock 2.048 MHz at 16 kHz; SPM1423 is spec'd
     * 1.0-3.25 MHz). The IDF default x64 leaves the PDM modulator's shaped
     * noise in the 4-8 kHz band: +8-10 dB hiss over speech on a StickC Plus
     * (2026-10-07), the "bad audio" with no codec involved. */
    pdm_cfg.clk_cfg.dn_sample_mode = (cfg && cfg->dsr_x64) ? I2S_PDM_DSR_8S : I2S_PDM_DSR_16S;
    err = i2s_channel_init_pdm_rx_mode(s_rx, &pdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init_pdm_rx_mode failed: 0x%x (clk=%d din=%d)", err, clk, din);
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "PDM mic ready: clk=%d din=%d rate=%d Hz slot=%s x%d (%u B/s)",
             clk, din, s_rate, left ? "L" : "R", (cfg && cfg->dsr_x64) ? 64 : 128,
             (unsigned)craw_mic_bytes_per_sec());
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
    if (s_streaming) return ESP_ERR_INVALID_STATE;
    if (out_samples) *out_samples = 0;

    size_t want = (size_t)seconds * (size_t)s_rate;
    if (want > max_samples) want = max_samples;   /* clamp to buffer */

    esp_err_t err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel_enable failed: 0x%x", err);
        return err;
    }

    settle();

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

int craw_mic_remove_dc(int16_t *buf, size_t n) {
    if (!n) return 0;
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += buf[i];
    int mean = (int)(sum / (int64_t)n);
    for (size_t i = 0; i < n; i++) {
        int v = buf[i] - mean;
        buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    return mean;
}

void craw_mic_hpf_init(craw_mic_hpf_t *f, float fc, int fs) {
    const float q = 0.7071f;
    float w0 = 2.0f * (float)M_PI * fc / (float)fs, c = cosf(w0), al = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + al;
    f->b0 = (1.0f + c) / 2.0f / a0; f->b1 = -(1.0f + c) / a0; f->b2 = f->b0;
    f->a1 = -2.0f * c / a0;         f->a2 = (1.0f - al) / a0;
    f->z1 = f->z2 = 0.0f;
}

float craw_mic_hpf_run(craw_mic_hpf_t *f, float x) {   /* transposed DF-II */
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

void craw_mic_hpf_block(craw_mic_hpf_t *f, int16_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float y = craw_mic_hpf_run(f, (float)buf[i]);
        buf[i] = (int16_t)(y > 32767.0f ? 32767 : y < -32768.0f ? -32768 : lrintf(y));
    }
}

int craw_mic_peak(const int16_t *buf, size_t n) {
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        int v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > peak) peak = v;
    }
    return peak > 32767 ? 32767 : peak;   /* -(-32768) */
}

esp_err_t craw_mic_stream_start(void) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (s_streaming) return ESP_OK;
    esp_err_t err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream enable failed: 0x%x", err);
        return err;
    }
    settle();
    s_streaming = true;
    return ESP_OK;
}

esp_err_t craw_mic_stream_read(int16_t *buf, size_t n, size_t *out_n, uint32_t timeout_ms) {
    if (out_n) *out_n = 0;
    if (!s_streaming) return ESP_ERR_INVALID_STATE;
    size_t got = 0;
    esp_err_t err = i2s_channel_read(s_rx, buf, n * sizeof(int16_t), &got,
                                     pdMS_TO_TICKS(timeout_ms));
    if (out_n) *out_n = got / sizeof(int16_t);
    return err;
}

esp_err_t craw_mic_stream_stop(void) {
    if (!s_streaming) return ESP_OK;
    s_streaming = false;
    return i2s_channel_disable(s_rx);
}

bool craw_mic_streaming(void) { return s_streaming; }
