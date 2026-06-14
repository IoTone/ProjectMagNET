#ifndef CRAW_MIC_H
#define CRAW_MIC_H
#define CRAW_MIC_VERSION "0.1.0"

/* craw_mic — SPM1423 PDM microphone capture for the M5StickC Plus.
 *
 * The StickC Plus has a digital PDM MEMS mic (SPM1423): clock on GPIO0,
 * data on GPIO34. This wraps the ESP-IDF I2S PDM-RX driver to deliver
 * 16-bit signed mono PCM at a configurable rate (default 16 kHz).
 *
 * No PSRAM on this board, so the record buffer is internal DRAM and the
 * caller owns it. Use craw_mic_max_record_seconds() to size a buffer
 * against the current free-heap budget before recording.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRAW_MIC_CLK_GPIO     0
#define CRAW_MIC_DIN_GPIO     34
#define CRAW_MIC_SAMPLE_RATE  16000   /* 16 kHz, 16-bit mono = 32 KB/s */

typedef struct {
    int clk_gpio;       /* 0 default handled as GPIO0 via flag below */
    int din_gpio;
    int sample_rate;    /* 0 → CRAW_MIC_SAMPLE_RATE */
} craw_mic_config_t;

/* Progress callback during a blocking record: fraction is 0.0..1.0. */
typedef void (*craw_mic_progress_cb_t)(float fraction, void *ctx);

/* Install the I2S PDM-RX channel (does not start capture). Idempotent. */
esp_err_t craw_mic_init(const craw_mic_config_t *cfg);
esp_err_t craw_mic_deinit(void);
bool      craw_mic_is_initialized(void);

/* Configured sample rate (Hz) and bytes-per-second (rate * 2, mono 16-bit). */
int       craw_mic_sample_rate(void);
size_t    craw_mic_bytes_per_sec(void);

/* How many whole seconds fit in `budget_bytes` at the current rate. */
int       craw_mic_max_record_seconds(size_t budget_bytes);

/* Record up to `seconds` of audio into `buf` (caller-allocated, room for
 * max_samples int16 samples). Blocks until done. Writes the sample count to
 * *out_samples. progress_cb (nullable) is called periodically. Enables the
 * channel for the duration and disables it after. */
esp_err_t craw_mic_record(int16_t *buf, size_t max_samples, int seconds,
                          size_t *out_samples,
                          craw_mic_progress_cb_t progress_cb, void *cb_ctx);

/* Peak absolute amplitude (0..32767) over a sample window — for level meters. */
int       craw_mic_peak(const int16_t *buf, size_t n);

#ifdef __cplusplus
}
#endif
#endif
