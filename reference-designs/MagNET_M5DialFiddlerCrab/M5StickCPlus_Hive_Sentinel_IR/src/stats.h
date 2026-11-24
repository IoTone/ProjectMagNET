#ifndef STATS_H
#define STATS_H

/* stats — room-entry counts and occupied time per calendar hour / day /
 * week (ISO, Monday start) / month, in local time. Pure C (libc time only),
 * host-testable (tools/test_logic.c).
 *
 * Each bucket remembers which period it belongs to (key). stats_roll()
 * compares that with the current local time and zeroes any bucket whose
 * period has ended, handing the closed values back so the caller can
 * publish them. Key 0 means "not known yet" (clock not synced): such a
 * bucket adopts the current period without being cleared, so entries
 * counted before SNTP lands are kept.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum { ST_HOUR, ST_DAY, ST_WEEK, ST_MONTH, ST_N };
#define ST_ALL ((1u << ST_N) - 1)

#define STATS_MAGIC 0x53543031u   /* "ST01" — bump when the layout changes */

typedef struct {
    uint32_t magic;
    int32_t  key[ST_N];
    uint32_t entries[ST_N];
    uint32_t occ_s[ST_N];       /* seconds with someone present */
} stats_t;

void        stats_init(stats_t *s);
int32_t     stats_key(int period, const struct tm *lt);
/* Returns a bitmask of periods that ended; their old values land in *closed. */
unsigned    stats_roll(stats_t *s, const struct tm *lt, stats_t *closed);
void        stats_add_entry(stats_t *s);
void        stats_add_occ(stats_t *s, uint32_t secs);
/* Zero the periods in mask. lt may be NULL (clock unknown: key → 0). */
void        stats_reset(stats_t *s, unsigned mask, const struct tm *lt);
const char *stats_period_name(int period);
/* Human label for a period key: 2026-10-26T15, 2026-10-26, 2026-W44, 2026-10. */
void        stats_label(int period, int32_t key, char *buf, size_t n);

#endif
