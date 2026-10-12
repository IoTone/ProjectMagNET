#ifndef CRAW_MIC_H
#define CRAW_MIC_H
#define CRAW_MIC_VERSION "0.2.0"

/* craw_mic — SPM1423 PDM microphone capture for the M5StickC Plus.
 *
 * Shared component (promoted from M5StickCPlus-Blinky_Voice_XR, H10 policy):
 * consumers are Voice_XR (one-shot record) and M5StickCPlus_Hive_Ear
 * (continuous stream). Note: CLK is on GPIO0 — a boot strap pin; only init
 * after boot.
 *
 * POWER: the SPM1423 is fed by AXP192 GPIO0 run as an LDO, which M5GFX does
 * NOT enable. The caller's board layer must write AXP192 0x91=0xF0 (3.3 V)
 * and 0x90=0x02 (GPIO0 = LDO) before recording — otherwise PCM reads a
 * constant ~-30935. The PDM output also carries a large DC offset; use
 * craw_mic_remove_dc() (or a high-pass) before measuring levels.
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
    /* PDM slot. The StickC Plus SPM1423 answers on the RIGHT slot (as
     * M5Unified reads it); IDF's mono default is LEFT, which samples the
     * idle clock edge and returns a near-constant ~-31000 (seen on HW
     * 2026-10-06). false (default) = RIGHT. */
    bool slot_left;
    /* PDM decimation. false (default) = x128 (2.048 MHz PDM clock at 16 kHz),
     * which keeps modulator noise out of 4-8 kHz; true = IDF default x64. */
    bool dsr_x64;
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

/* Subtract the window's mean in place; returns the removed DC offset. */
int       craw_mic_remove_dc(int16_t *buf, size_t n);

/* Voice-band high-pass (2nd-order Butterworth biquad). The classic ESP32 has
 * no PDM high-pass in hardware and the SPM1423's DC offset drifts by
 * hundreds of counts per second — measured 94-99 % of signal energy below
 * 60 Hz — which swamps distant speech in any level meter. Stateful, so it
 * can run across stream frames; subtract the DC first (remove_dc or a
 * tracker) to avoid a start thump. */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } craw_mic_hpf_t;
void      craw_mic_hpf_init(craw_mic_hpf_t *f, float cutoff_hz, int sample_rate);
float     craw_mic_hpf_run(craw_mic_hpf_t *f, float x);
void      craw_mic_hpf_block(craw_mic_hpf_t *f, int16_t *buf, size_t n);  /* in place */

/* Peak absolute amplitude (0..32767) over a sample window — for level meters. */
int       craw_mic_peak(const int16_t *buf, size_t n);

/* ---- Streaming (0.2.0) ----
 * Continuous capture for callers that run their own task (e.g. an encoder
 * feeding BLE). start() enables the channel and discards the settling
 * transient; read() blocks for exactly n samples (or timeout); stop()
 * disables the channel. Mutually exclusive with craw_mic_record(). */
esp_err_t craw_mic_stream_start(void);
esp_err_t craw_mic_stream_read(int16_t *buf, size_t n, size_t *out_n, uint32_t timeout_ms);
esp_err_t craw_mic_stream_stop(void);
bool      craw_mic_streaming(void);

#ifdef __cplusplus
}
#endif
#endif
