/*
 * craw_audio_synth — fixed-point sine synthesizer.
 *
 * One LUT (256 entries × int16) lives in flash. Phase accumulates in
 * Q16.16; the top 8 bits index the LUT, next 8 bits linearly interpolate
 * between adjacent entries. Outputs s16 samples at 16 kHz.
 *
 * AM modulation reuses the same LUT for the modulator at a slow rate
 * (1..20 Hz typical for a siren effect).
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include "craw_audio.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* Sine LUT: one full period, 256 entries, signed 16-bit. Generated at
 * startup-ish — actually we just precompute at compile time via a small
 * table. To keep the source self-contained we generate at first use. */

static int16_t SINE_LUT[256];
static bool    s_lut_ready = false;

static void lut_init(void) {
    if (s_lut_ready) return;
    for (int i = 0; i < 256; i++) {
        SINE_LUT[i] = (int16_t)(sinf((float)i * (2.0f * (float)M_PI / 256.0f)) * 32000.0f);
    }
    s_lut_ready = true;
}

/* Sample one element from the LUT given a Q16.16 phase, with linear
 * interpolation between adjacent integer indices. */
static inline int16_t lut_sample(uint32_t phase_q1616) {
    uint32_t idx_int  = (phase_q1616 >> 16) & 0xFF;
    uint32_t idx_next = (idx_int + 1) & 0xFF;
    uint32_t frac8    = (phase_q1616 >> 8) & 0xFF;
    int32_t  a = SINE_LUT[idx_int];
    int32_t  b = SINE_LUT[idx_next];
    /* a + (b-a) * frac/256 */
    return (int16_t)(a + (((b - a) * (int32_t)frac8) >> 8));
}

/* Phase increment per sample for `freq` Hz at the configured sample rate.
 * One full LUT (256 entries) per cycle, so the increment in Q16.16 is:
 *     freq * 256 / SR  (in fractional table positions)
 *   = (freq * 256 << 16) / SR
 */
static inline uint32_t phase_inc(uint32_t freq) {
    return (uint32_t)((((uint64_t)freq) << 24) / CRAW_AUDIO_SAMPLE_RATE);
}

void craw_audio_synth_lut_ensure(void) { lut_init(); }

/* Public render functions called by craw_audio_i2s.c.
 * Each fills `out` with `n_samples` int16 mono samples and returns the
 * updated phase + the number of samples actually written. */

uint32_t craw_audio_synth_tone(int16_t *out, size_t n,
                               uint32_t phase, uint32_t freq, float gain) {
    lut_init();
    uint32_t inc = phase_inc(freq);
    int16_t  g   = (int16_t)(gain * 32767.0f);
    if (g < 0)     g = 0;
    if (g > 32767) g = 32767;
    for (size_t i = 0; i < n; i++) {
        int32_t s = lut_sample(phase);
        out[i] = (int16_t)((s * g) >> 15);
        phase += inc;
    }
    return phase;
}

/* Linear sweep f0 → f1 over the FULL `total_samples` of the segment,
 * but render only the chunk [sample_offset .. sample_offset + n).
 * Caller drives sample_offset across multiple chunks. */
uint32_t craw_audio_synth_sweep(int16_t *out, size_t n,
                                uint32_t phase,
                                uint32_t f0, uint32_t f1,
                                size_t sample_offset, size_t total_samples,
                                float gain) {
    lut_init();
    int16_t g = (int16_t)(gain * 32767.0f);
    if (g < 0)     g = 0;
    if (g > 32767) g = 32767;
    int32_t df = (int32_t)f1 - (int32_t)f0;
    for (size_t i = 0; i < n; i++) {
        size_t pos = sample_offset + i;
        uint32_t freq = (total_samples > 0)
            ? (uint32_t)((int32_t)f0 + (df * (int32_t)pos) / (int32_t)total_samples)
            : f0;
        uint32_t inc = phase_inc(freq);
        int32_t s = lut_sample(phase);
        out[i] = (int16_t)((s * g) >> 15);
        phase += inc;
    }
    return phase;
}

/* AM-modulated tone: carrier at fc, scaled by (0.5 + 0.5*sin(2π·fm·t)).
 * Modulator phase tracked separately by caller. */
uint32_t craw_audio_synth_am(int16_t *out, size_t n,
                             uint32_t phase_carrier, uint32_t *phase_mod,
                             uint32_t fc, uint32_t fm, float gain) {
    lut_init();
    uint32_t inc_c = phase_inc(fc);
    uint32_t inc_m = phase_inc(fm);
    int16_t  g     = (int16_t)(gain * 32767.0f);
    if (g < 0)     g = 0;
    if (g > 32767) g = 32767;
    uint32_t pm = phase_mod ? *phase_mod : 0;
    for (size_t i = 0; i < n; i++) {
        int32_t s = lut_sample(phase_carrier);
        /* Modulator output is signed; map to [0, 1] amplitude scale. */
        int32_t m = lut_sample(pm);
        int32_t scale = 16384 + (m / 2);   /* 0.5 + 0.5*m  in q15 */
        int32_t v = (s * g) >> 15;          /* base scaled by gain */
        out[i] = (int16_t)((v * scale) >> 15);
        phase_carrier += inc_c;
        pm += inc_m;
    }
    if (phase_mod) *phase_mod = pm;
    return phase_carrier;
}

/* Apply a linear gain envelope across `total_samples` of a segment to
 * the chunk [sample_offset .. sample_offset + n). Operates in place. */
void craw_audio_synth_apply_envelope(int16_t *buf, size_t n,
                                     size_t sample_offset, size_t total_samples,
                                     float gain_start, float gain_end) {
    if (total_samples == 0) return;
    float dg = gain_end - gain_start;
    /* gain at any sample position = gain_start + dg * (pos / total) */
    for (size_t i = 0; i < n; i++) {
        size_t pos = sample_offset + i;
        float g = gain_start + dg * ((float)pos / (float)total_samples);
        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
        buf[i] = (int16_t)((int32_t)buf[i] * (int32_t)(g * 32767.0f) >> 15);
    }
}

void craw_audio_synth_silence(int16_t *out, size_t n) {
    memset(out, 0, n * sizeof(int16_t));
}
