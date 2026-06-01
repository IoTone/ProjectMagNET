#ifndef CRAW_AUDIO_H
#define CRAW_AUDIO_H

/* craw_audio — I2S-based mono audio output for MagNET Role 12 (Boombox).
 *
 * v1: 16 kHz 16-bit mono via I2S0, software synthesizer (sine LUT) producing
 *     pure tones, frequency sweeps, AM-modulated tones, and multi-segment
 *     patterns with per-segment gain envelopes. No PSRAM dependency, no
 *     filesystem, no embedded sample data — patterns are compositions of
 *     primitives in flash rodata.
 *
 * Hardware target: XIAO ESP32-S3 socketed onto the Seeed ReSpeaker Lite
 *     Voice Kit carrier. The carrier exposes I2S MCLK/BCLK/WS/DOUT to
 *     either the on-board codec or the XMOS XU316 host bridge depending
 *     on jumper config. Pin numbers come from craw_audio_pins_t at init
 *     so swapping carriers is one struct.
 *
 * Concurrency: a single internal `audio_render` task drains a FreeRTOS
 *     queue of play requests and writes samples to the I2S DMA. All
 *     public play_* functions push onto that queue and return — they
 *     never block on audio. craw_audio_stop() drains the queue.
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRAW_AUDIO_SAMPLE_RATE      16000   /* Hz; 4 kHz Nyquist headroom */
#define CRAW_AUDIO_QUEUE_DEPTH      16      /* play requests in flight */
#define CRAW_AUDIO_PATTERN_SEGS_MAX 32
#define CRAW_AUDIO_DMA_FRAMES       512     /* per DMA buffer */
#define CRAW_AUDIO_DMA_BUFFERS      4

typedef struct {
    int bclk;       /* I2S bit clock */
    int ws;         /* word select / LRCK */
    int dout;       /* serial data out */
    int pwr_en;     /* amp enable; -1 if not used */
    bool pwr_en_active_high;
} craw_audio_pins_t;

/* Pattern segment kinds. */
typedef enum {
    CRAW_AUDIO_SEG_TONE  = 0,   /* steady sine at f0 */
    CRAW_AUDIO_SEG_SWEEP = 1,   /* linear freq sweep f0 -> f1 */
    CRAW_AUDIO_SEG_AM    = 2,   /* sine carrier f0 amplitude-modulated by f1 */
    CRAW_AUDIO_SEG_SLEEP = 3,   /* silence */
} craw_audio_seg_kind_t;

/* One segment of a pattern. gain is 0.0..1.0 (post-volume scale).
 * If gain_end > 0, the segment linearly interpolates gain → gain_end
 * across its duration (envelope). gain_end == 0 + gain == 0 means
 * "silent segment" not "envelope to zero" — use SLEEP for silence. */
typedef struct {
    craw_audio_seg_kind_t kind;
    uint16_t f0;          /* Hz */
    uint16_t f1;          /* Hz; sweep target or AM mod freq */
    uint16_t ms;
    float    gain;        /* start gain (or constant if gain_end==0) */
    float    gain_end;    /* end gain for envelope; 0 = no envelope */
} craw_audio_seg_t;

/* Init the audio subsystem. Caller-owned pins struct copied internally.
 * Returns 0 on success, -1 on bad arg, -2 on I2S init failure, -3 on
 * task spawn failure. Idempotent: a second call is a no-op. */
int  craw_audio_init(const craw_audio_pins_t *pins);

/* Tear down (stops render task, releases I2S). Mostly for tests. */
void craw_audio_deinit(void);

/* Power amplifier on/off — toggles pwr_en if configured. Audio playback
 * still works with amp off but you'll hear nothing on the speaker. */
void craw_audio_amp_set(bool on);
bool craw_audio_amp_get(void);

/* Master volume 0..100. Persisted by caller (we don't touch NVS here). */
void craw_audio_volume_set(int vol_pct);
int  craw_audio_volume_get(void);

/* Single-segment helpers. Each enqueues one play request. */
int  craw_audio_play_tone (uint16_t freq, uint16_t ms, float gain);
int  craw_audio_play_sweep(uint16_t f0, uint16_t f1, uint16_t ms, float gain);
int  craw_audio_play_am   (uint16_t fc, uint16_t fm, uint16_t ms, float gain);
int  craw_audio_play_sleep(uint16_t ms);

/* Multi-segment pattern. The segs array is copied into the queue
 * (n_segs ≤ CRAW_AUDIO_PATTERN_SEGS_MAX). Returns 0 on accept. */
int  craw_audio_play_pattern(const craw_audio_seg_t *segs, int n_segs);

/* Drain the queue, abort whatever's currently rendering (silence
 * resumes within ~1 DMA buffer = ~32 ms). */
void craw_audio_stop(void);

/* Stats for `audio-status` Forth word. */
typedef struct {
    bool     amp_on;
    int      volume_pct;
    int      queue_depth;
    bool     rendering;
    uint32_t total_segments_played;
    uint64_t total_samples_out;
} craw_audio_stats_t;
void craw_audio_stats(craw_audio_stats_t *out);

/* ---- Built-in notification recipes ----
 *
 * Implementation lives in craw_audio_patterns.c; declarations here so
 * main.c can wire them as Forth words and bundles can call them too.
 */
int craw_audio_play_alert (void);   /* three rising chirps */
int craw_audio_play_notify(void);   /* two-beep ding */
int craw_audio_play_warn  (void);   /* AM-modulated alarm beep */
int craw_audio_play_error (void);   /* descending sweep + hold */
int craw_audio_play_siren (void);   /* two-cycle wail 500↔1500 Hz */
int craw_audio_play_yelp  (void);   /* fast 3-cycle yelp variant */
int craw_audio_play_nee_naw(void);  /* European two-tone 950/750 Hz */
int craw_audio_play_air_raid(void); /* slow rise + hold + slow fall */
int craw_audio_play_sunrise(void);  /* C-major arpeggio with envelope */

#ifdef __cplusplus
}
#endif
#endif
