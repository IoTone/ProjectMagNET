/*
 * craw_audio_i2s — I2S0 master TX + render task.
 *
 * One render task drains a queue of play requests. For each request it
 * walks the segments, calls into craw_audio_synth.c to fill a working
 * buffer, applies the envelope, then writes to the I2S DMA via
 * i2s_channel_write. The DMA double-buffers behind us, so as long as we
 * stay ahead of the sample rate the audio is glitch-free.
 *
 * Design notes:
 *   - Render buffer size = 1 DMA frame (CRAW_AUDIO_DMA_FRAMES samples)
 *     so we hand off in chunks bounded enough to react to stop() within
 *     ~32 ms at 16 kHz.
 *   - The sample-position cursor for SWEEP / envelope is maintained
 *     inside the segment loop so chunked renders compose correctly.
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include "craw_audio.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

static const char *TAG = "craw_audio";

/* Render-side helpers exposed by craw_audio_synth.c */
extern void     craw_audio_synth_lut_ensure(void);
extern uint32_t craw_audio_synth_tone (int16_t *out, size_t n,
                                       uint32_t phase, uint32_t freq, float gain);
extern uint32_t craw_audio_synth_sweep(int16_t *out, size_t n,
                                       uint32_t phase, uint32_t f0, uint32_t f1,
                                       size_t sample_offset, size_t total_samples,
                                       float gain);
extern uint32_t craw_audio_synth_am   (int16_t *out, size_t n,
                                       uint32_t phase_carrier, uint32_t *phase_mod,
                                       uint32_t fc, uint32_t fm, float gain);
extern void     craw_audio_synth_apply_envelope(int16_t *buf, size_t n,
                                                size_t sample_offset, size_t total_samples,
                                                float gain_start, float gain_end);
extern void     craw_audio_synth_silence(int16_t *out, size_t n);

/* ---- A play request ---- */
typedef struct {
    int               n_segs;
    craw_audio_seg_t  segs[CRAW_AUDIO_PATTERN_SEGS_MAX];
} req_t;

/* ---- Module state ---- */
static struct {
    bool                init_done;
    craw_audio_pins_t   pins;
    i2s_chan_handle_t   tx;
    QueueHandle_t       q;
    TaskHandle_t        render_task;
    volatile bool       abort_current;
    volatile bool       rendering;
    volatile bool       amp_on;
    volatile int        volume_pct;
    volatile uint32_t   total_segments;
    volatile uint64_t   total_samples;
} S;

/* ---- Internal helpers ---- */

static inline float master_gain(void) {
    /* Scale 0..100 → 0.0..1.0. */
    int v = S.volume_pct;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    return (float)v / 100.0f;
}

static int i2s_setup(const craw_audio_pins_t *pins) {
    /* SLAVE role — the XU316 on the ReSpeaker carrier generates BCLK + WS.
     * STEREO 32-bit slots — that's what the XU316/codec pipe expects.
     * Our synth produces 16-bit mono; we expand to 32-bit and duplicate
     * across L+R in the render path. */
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    cc.dma_desc_num   = CRAW_AUDIO_DMA_BUFFERS;
    cc.dma_frame_num  = CRAW_AUDIO_DMA_FRAMES;
    cc.auto_clear     = true;
    if (i2s_new_channel(&cc, &S.tx, NULL) != ESP_OK) return -1;

    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CRAW_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = pins->bclk,
            .ws   = pins->ws,
            .dout = pins->dout,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };
    if (i2s_channel_init_std_mode(S.tx, &sc) != ESP_OK) {
        i2s_del_channel(S.tx); S.tx = NULL;
        return -2;
    }
    if (i2s_channel_enable(S.tx) != ESP_OK) {
        i2s_del_channel(S.tx); S.tx = NULL;
        return -3;
    }
    return 0;
}

static void amp_apply(bool on) {
    if (S.pins.pwr_en < 0) return;
    int level = on
        ? (S.pins.pwr_en_active_high ? 1 : 0)
        : (S.pins.pwr_en_active_high ? 0 : 1);
    gpio_set_level((gpio_num_t)S.pins.pwr_en, level);
    S.amp_on = on;
}

/* Render one segment, in chunks of CRAW_AUDIO_DMA_FRAMES samples.
 * Returns 0 normally, -1 if aborted mid-segment.
 *
 * The synth produces 16-bit mono samples in `buf`. The XU316/codec pipe
 * on the ReSpeaker Lite carrier wants 32-bit stereo, so we expand each
 * mono sample to {L=s32, R=s32} in `i2s_buf` before writing. The
 * conversion is `(int32_t)s16 << 16` which sign-extends the 16-bit
 * sample into the upper bits of a 32-bit slot. */
static int render_segment(const craw_audio_seg_t *seg) {
    size_t total_samples =
        (size_t)((uint32_t)seg->ms * CRAW_AUDIO_SAMPLE_RATE / 1000);
    if (total_samples == 0) return 0;

    /* Static — only the single render task uses these, and putting them
     * on the stack would push the 4 KB task stack over the edge (the
     * stereo expansion buffer alone is 4 KB). */
    static int16_t buf[CRAW_AUDIO_DMA_FRAMES];
    static int32_t i2s_buf[CRAW_AUDIO_DMA_FRAMES * 2];
    uint32_t phase    = 0;
    uint32_t phase_m  = 0;
    size_t   off      = 0;
    float    gain_eff = seg->gain * master_gain();
    float    gain_end_eff = (seg->gain_end > 0.0f)
                              ? seg->gain_end * master_gain()
                              : 0.0f;
    bool     envelope = (seg->gain_end > 0.0f);

    while (off < total_samples) {
        if (S.abort_current) return -1;
        size_t n = total_samples - off;
        if (n > CRAW_AUDIO_DMA_FRAMES) n = CRAW_AUDIO_DMA_FRAMES;

        switch (seg->kind) {
            case CRAW_AUDIO_SEG_TONE:
                phase = craw_audio_synth_tone(buf, n, phase, seg->f0,
                                               envelope ? seg->gain : gain_eff);
                if (envelope) {
                    craw_audio_synth_apply_envelope(buf, n, off, total_samples,
                                                     seg->gain * master_gain(),
                                                     gain_end_eff);
                }
                break;
            case CRAW_AUDIO_SEG_SWEEP:
                phase = craw_audio_synth_sweep(buf, n, phase, seg->f0, seg->f1,
                                                off, total_samples,
                                                envelope ? seg->gain : gain_eff);
                if (envelope) {
                    craw_audio_synth_apply_envelope(buf, n, off, total_samples,
                                                     seg->gain * master_gain(),
                                                     gain_end_eff);
                }
                break;
            case CRAW_AUDIO_SEG_AM:
                phase = craw_audio_synth_am(buf, n, phase, &phase_m,
                                             seg->f0, seg->f1,
                                             envelope ? seg->gain : gain_eff);
                if (envelope) {
                    craw_audio_synth_apply_envelope(buf, n, off, total_samples,
                                                     seg->gain * master_gain(),
                                                     gain_end_eff);
                }
                break;
            case CRAW_AUDIO_SEG_SLEEP:
            default:
                craw_audio_synth_silence(buf, n);
                break;
        }

        /* Expand mono int16 → stereo int32 for the XU316 pipeline. */
        for (size_t i = 0; i < n; i++) {
            int32_t s32 = ((int32_t)buf[i]) << 16;
            i2s_buf[i * 2 + 0] = s32;   /* L */
            i2s_buf[i * 2 + 1] = s32;   /* R */
        }
        size_t bytes_written = 0;
        i2s_channel_write(S.tx, i2s_buf, n * 2 * sizeof(int32_t),
                          &bytes_written, portMAX_DELAY);
        S.total_samples += n;
        off += n;
    }
    S.total_segments++;
    return 0;
}

static void render_task(void *arg) {
    (void)arg;
    craw_audio_synth_lut_ensure();
    req_t *req = NULL;
    for (;;) {
        if (xQueueReceive(S.q, &req, portMAX_DELAY) != pdTRUE) continue;
        if (!req) continue;
        S.rendering = true;
        S.abort_current = false;
        for (int i = 0; i < req->n_segs; i++) {
            if (S.abort_current) break;
            render_segment(&req->segs[i]);
        }
        free(req);
        S.rendering = false;
    }
}

/* ---- Public API ---- */

int craw_audio_init(const craw_audio_pins_t *pins) {
    if (S.init_done) return 0;
    if (!pins) return -1;
    S.pins = *pins;
    S.volume_pct = 60;

    if (S.pins.pwr_en >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << S.pins.pwr_en),
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io);
        amp_apply(false);
    }

    if (i2s_setup(&S.pins) != 0) {
        ESP_LOGE(TAG, "i2s setup failed");
        return -2;
    }

    S.q = xQueueCreate(CRAW_AUDIO_QUEUE_DEPTH, sizeof(req_t *));
    if (!S.q) return -3;

    if (xTaskCreate(render_task, "audio_render", 4096, NULL, 5, &S.render_task)
        != pdPASS) {
        vQueueDelete(S.q);
        S.q = NULL;
        return -3;
    }

    S.init_done = true;
    ESP_LOGI(TAG, "I2S0 16kHz mono ready (bclk=%d ws=%d dout=%d amp=%d)",
             pins->bclk, pins->ws, pins->dout, pins->pwr_en);
    return 0;
}

void craw_audio_deinit(void) {
    if (!S.init_done) return;
    craw_audio_stop();
    if (S.render_task) { vTaskDelete(S.render_task); S.render_task = NULL; }
    if (S.q)           { vQueueDelete(S.q);           S.q = NULL; }
    if (S.tx) {
        i2s_channel_disable(S.tx);
        i2s_del_channel(S.tx);
        S.tx = NULL;
    }
    amp_apply(false);
    S.init_done = false;
}

void craw_audio_amp_set(bool on) { amp_apply(on); }
bool craw_audio_amp_get(void)    { return S.amp_on; }

void craw_audio_volume_set(int v) {
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    S.volume_pct = v;
}
int craw_audio_volume_get(void) { return S.volume_pct; }

static int enqueue(const craw_audio_seg_t *segs, int n) {
    if (!S.init_done || n <= 0 || n > CRAW_AUDIO_PATTERN_SEGS_MAX) return -1;
    req_t *r = malloc(sizeof(*r));
    if (!r) return -2;
    r->n_segs = n;
    memcpy(r->segs, segs, n * sizeof(craw_audio_seg_t));
    if (xQueueSend(S.q, &r, 0) != pdTRUE) { free(r); return -3; }
    return 0;
}

int craw_audio_play_tone(uint16_t f, uint16_t ms, float gain) {
    craw_audio_seg_t s = { .kind = CRAW_AUDIO_SEG_TONE, .f0 = f, .ms = ms, .gain = gain };
    return enqueue(&s, 1);
}
int craw_audio_play_sweep(uint16_t f0, uint16_t f1, uint16_t ms, float gain) {
    craw_audio_seg_t s = { .kind = CRAW_AUDIO_SEG_SWEEP, .f0 = f0, .f1 = f1, .ms = ms, .gain = gain };
    return enqueue(&s, 1);
}
int craw_audio_play_am(uint16_t fc, uint16_t fm, uint16_t ms, float gain) {
    craw_audio_seg_t s = { .kind = CRAW_AUDIO_SEG_AM, .f0 = fc, .f1 = fm, .ms = ms, .gain = gain };
    return enqueue(&s, 1);
}
int craw_audio_play_sleep(uint16_t ms) {
    craw_audio_seg_t s = { .kind = CRAW_AUDIO_SEG_SLEEP, .ms = ms };
    return enqueue(&s, 1);
}
int craw_audio_play_pattern(const craw_audio_seg_t *segs, int n) {
    return enqueue(segs, n);
}

void craw_audio_stop(void) {
    if (!S.init_done) return;
    S.abort_current = true;
    /* Drain queue */
    req_t *r = NULL;
    while (xQueueReceive(S.q, &r, 0) == pdTRUE) free(r);
}

void craw_audio_stats(craw_audio_stats_t *out) {
    if (!out) return;
    out->amp_on                = S.amp_on;
    out->volume_pct            = S.volume_pct;
    out->queue_depth           = S.q ? (int)uxQueueMessagesWaiting(S.q) : 0;
    out->rendering             = S.rendering;
    out->total_segments_played = S.total_segments;
    out->total_samples_out     = S.total_samples;
}
