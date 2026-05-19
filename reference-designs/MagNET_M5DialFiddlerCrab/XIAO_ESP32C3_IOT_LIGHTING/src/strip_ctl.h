/*
 * strip_ctl.h - shared LED-strip control surface
 *
 * The render task (main.c) owns the RMT strip and continuously draws the
 * current state. The HTTP layer (http_strip.c) reads/updates that state.
 * Until the first HTTP command (or `strip-engage` at the REPL) the task
 * stays disengaged so the Phase-1 Forth self-test still owns the strip.
 */
#ifndef STRIP_CTL_H
#define STRIP_CTL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PAT_SOLID = 0,
    PAT_BREATHING,
    PAT_RAINBOW,
    PAT_CHASE,
    PAT_TWINKLE,
    PAT_COUNT
} strip_pattern_t;

/* Index-aligned with strip_pattern_t; matches UC2 NEOPIXEL_PATTERNS. */
extern const char *const STRIP_PATTERN_NAMES[PAT_COUNT];

typedef struct {
    bool            on;
    int             brightness_pct;   /* 0..100 */
    uint8_t         r, g, b;          /* 0..255 */
    strip_pattern_t pattern;
    int             pattern_speed_pct;/* 0..100 */
    int             led_count;        /* fixed: NUM_PIXELS */
    int64_t         last_changed_us;
} strip_state_t;

/* Create the mutex + render task. Call once after the strip driver init. */
void strip_ctl_init(void);

/* Snapshot the current state. */
void strip_get_state(strip_state_t *out);

/* Partial update — pass NULL for any field that should not change.
 * Engages the render task (HTTP/REPL takes the strip from the self-test). */
void strip_set(const bool *on,
               const int *brightness_pct,
               const uint8_t rgb[3],
               const strip_pattern_t *pattern,
               const int *pattern_speed_pct);

/* -1 if name is not a known pattern. */
int strip_pattern_from_name(const char *name);

#endif /* STRIP_CTL_H */
