/*
 * craw_mr60bha2 — ESP-IDF UART driver for the Seeed MR60BHA2 60GHz mmWave
 * heart-rate / breathing / multi-target presence radar.
 *
 * The radar is a sealed module that runs its own DSP firmware and emits
 * results as binary frames over UART at 115200 8N1, using Seeed's
 * "Tiny Frame Interface" (SOF + ID + LEN + TYPE + header-cksum + payload + data-cksum).
 *
 * This driver:
 *   - Configures the chosen UART
 *   - Spawns a background task that reads bytes, finds frame boundaries,
 *     validates the two XOR checksums, and dispatches per frame type
 *   - Maintains a thread-safe shared state struct + 60-entry HR/RR
 *     ring buffers (1 sample/min, intended for SNTP-synced timestamps)
 *   - Exposes a clean C API consumed by Forth words, the HTTP server,
 *     and (eventually) the WS2812 LED indicator
 *
 * Protocol reference: Seeed-Arduino-mmWave (Love4yzp/Seeed-mmWave-library).
 * Spec reference:    ProjectMagNET/specs/MagNET-Vitals-E4TH-proposal.md §4.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CRAW_MR60_MAX_TARGETS        3
#define CRAW_MR60_HISTORY_LEN        60   // 1 sample/min × 60 min — HR / RR
#define CRAW_MR60_PHASE_HISTORY_LEN  200  // continuous phase waveform — pushed every frame

typedef struct {
    float   x_m;             // metres in the radar's frame
    float   y_m;
    int32_t dop_index;       // doppler velocity bin
    int32_t cluster_index;
} craw_mr60_target_t;

typedef struct {
    /* Vitals */
    float    bpm;            // 0 if never received
    float    rpm;            // 0 if never received
    float    total_phase;
    float    breath_phase;
    float    heart_phase;
    float    distance_m;     // valid only when range_flag != 0
    uint32_t range_flag;

    /* Presence */
    bool     present;

    /* Multi-target */
    craw_mr60_target_t targets[CRAW_MR60_MAX_TARGETS];
    size_t   target_count;

    /* Firmware version (when reported) */
    uint32_t fw_version;

    /* Timestamps (esp_timer_get_time microseconds) of latest update per field */
    int64_t  bpm_updated_us;
    int64_t  rpm_updated_us;
    int64_t  presence_updated_us;
    int64_t  targets_updated_us;
    int64_t  any_frame_us;
} craw_mr60_state_t;

/**
 * Initialize the UART and start the background parser task.
 * Call once. Pins are GPIO numbers; pass -1 for tx_gpio if the radar
 * is read-only (no command channel needed).
 */
esp_err_t craw_mr60_init(uart_port_t port, int rx_gpio, int tx_gpio);

/** Stop the parser task and release the UART driver. */
void craw_mr60_deinit(void);

/** Snapshot copy of the latest state. Thread-safe. */
void craw_mr60_get_state(craw_mr60_state_t *out);

/** Convenience scalars (return 0 if never received). */
float craw_mr60_get_bpm(void);
float craw_mr60_get_rpm(void);
bool  craw_mr60_get_presence(void);

/** Copy current target list into `out` (capacity CRAW_MR60_MAX_TARGETS). */
size_t craw_mr60_get_targets(craw_mr60_target_t out[CRAW_MR60_MAX_TARGETS]);

/**
 * Copy chronologically-ordered HR / RR history into the caller's buffers.
 * `cap` is the buffer capacity; returns count actually written.
 * Timestamps are esp_timer-derived milliseconds; if the caller has done
 * SNTP sync, they should rebase to wall-clock externally.
 */
size_t craw_mr60_get_hr_history(uint64_t *t_ms, float *bpm, size_t cap);
size_t craw_mr60_get_rr_history(uint64_t *t_ms, float *rpm, size_t cap);

/**
 * Continuous phase waveform history (heart, breath, total).
 * Pushed every HeartBreathPhase frame — radar emits these at ~10 Hz, so
 * 200 samples ≈ 20 s of live waveform suitable for the streamgraph mark.
 * Any of `t_ms`, `heart`, `breath`, `total` may be NULL.
 */
size_t craw_mr60_get_phase_history(
    uint64_t *t_ms,
    float    *heart,
    float    *breath,
    float    *total,
    size_t    cap);

/* ─── Self-test diagnostics ─────────────────────────────────────────── */

typedef struct {
    uint64_t bytes_received;        /* total bytes from UART since init */
    uint64_t frames_valid;          /* frames that passed both checksums */
    uint32_t header_cksum_fail;     /* candidates rejected by header checksum */
    uint32_t data_cksum_fail;       /* candidates rejected by data checksum */
    uint32_t implausible_len;       /* candidates rejected by impossible LEN field */
    uint32_t unknown_type;          /* valid frames whose TYPE we didn't recognize */
    int64_t  first_frame_us;        /* esp_timer_get_time of first valid frame, 0 if none */
} craw_mr60_diagnostics_t;

void craw_mr60_get_diagnostics(craw_mr60_diagnostics_t *out);

/**
 * Wait up to `timeout_ms` for the radar to emit its first valid frame.
 * Returns true if a frame arrived in time. Useful as a power-on self-test.
 */
bool craw_mr60_self_test(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
