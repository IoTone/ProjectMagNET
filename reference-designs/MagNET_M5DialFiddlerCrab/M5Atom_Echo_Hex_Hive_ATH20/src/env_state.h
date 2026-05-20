/*
 * env_state.h - shared accessor for the latest AHT20 reading
 *
 * The sensor task in main.c samples every ~2 s and updates this state
 * (mutex-protected). HTTP handlers + Forth FFI words read it via
 * env_state_get(). Calibration offsets (NVS-backed) are applied in
 * env_state_set() so consumers always see corrected values.
 */
#ifndef ENV_STATE_H
#define ENV_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool    valid;
    float   t_c;
    float   rh_pct;
    int64_t ts_us;      /* esp_timer_get_time() of the latest sample */
} env_reading_t;

void env_state_init(void);
void env_state_set(float t_c, float rh_pct);
void env_state_get(env_reading_t *out);

/* Calibration offsets persisted in NVS (boombox-vol pattern). */
void env_set_cal(float t_offset_c, float rh_offset_pct);
void env_get_cal(float *t_offset_c, float *rh_offset_pct);

/* 60-sample history (≈ last 60 min at one push/min). Matches the
 * mock-join-server's `/light/history` shape so the existing live-line
 * chart in d3-spatial renders without changes. */
#define ENV_HIST_LEN 60
typedef struct { int64_t t_ms; float v; } env_sample_t;

/* Copy up to max_n recent samples into out (oldest → newest).
 * Returns the number copied. */
int env_history_temperature(env_sample_t *out, int max_n);
int env_history_humidity   (env_sample_t *out, int max_n);

#endif /* ENV_STATE_H */
