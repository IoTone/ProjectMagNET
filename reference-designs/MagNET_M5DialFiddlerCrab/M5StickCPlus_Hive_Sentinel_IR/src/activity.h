#ifndef ACTIVITY_H
#define ACTIVITY_H

/* activity — the two time series the Sentinel publishes. Pure C, time
 * passed in, host-testable (tools/test_logic.c).
 *
 *   history: occupied seconds per 15-min bin over the last 24 h (96 bins),
 *            aligned to the wall clock (key = unix time / 900).
 *   recent:  motion seen per 10-s bin over the last 5 min (30 bins), as
 *            milliseconds of active PIR output, on the monotonic clock.
 *
 * Both rings keep the newest bin last; advancing shifts older bins left and
 * zeroes the new ones.
 */

#include <stdbool.h>
#include <stdint.h>

#define HIST_BINS   96
#define HIST_BIN_S  900
#define RECENT_BINS 30
#define RECENT_BIN_MS 10000

#define HIST_MAGIC 0x48493031u        /* "HI01" */

typedef struct {
    uint32_t magic;
    int32_t  key;                     /* unix time / 900 of the newest bin; 0 = clock unknown */
    uint16_t occ_s[HIST_BINS];
} hist_t;

typedef struct {
    int64_t  slot;                    /* now_ms / 10000 of the newest bin */
    uint16_t active_ms[RECENT_BINS];
} recent_t;

void hist_init(hist_t *h);
/* Move to the bin for key_now. A key of 0 (clock unknown) adopts key_now
 * without shifting, so time counted before SNTP stays in the current bin.
 * Returns true when the newest bin changed. */
bool hist_advance(hist_t *h, int32_t key_now);
void hist_add(hist_t *h, uint32_t secs);       /* to the newest bin, capped at 900 */

void     recent_init(recent_t *r, int64_t now_ms);
void     recent_advance(recent_t *r, int64_t now_ms);
void     recent_add(recent_t *r, int64_t now_ms, uint32_t active_ms);
uint32_t recent_total_ms(const recent_t *r);

#endif
