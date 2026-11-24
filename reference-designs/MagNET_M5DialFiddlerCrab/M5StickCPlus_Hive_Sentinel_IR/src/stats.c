// stats — see stats.h.

#include "stats.h"
#include <stdio.h>
#include <string.h>

void stats_init(stats_t *s) {
    memset(s, 0, sizeof(*s));
    s->magic = STATS_MAGIC;
}

// ISO-8601 week: weeks start Monday; week 1 holds the year's first Thursday.
// Returns year*100 + week. strftime("%G%V") would do, but newlib's support
// varies, so compute it.
static int32_t iso_week_key(const struct tm *lt) {
    int wday = (lt->tm_wday + 6) % 7;                 // Mon=0 .. Sun=6
    int year = lt->tm_year + 1900;
    int week = (lt->tm_yday - wday + 10) / 7;         // 0 or 53 need fixing
    if (week < 1) {
        // Belongs to the last week of the previous year.
        year--;
        int pyday = lt->tm_yday + 365 + (((year % 4 == 0 && year % 100) || year % 400 == 0) ? 1 : 0);
        week = (pyday - wday + 10) / 7;
    } else if (week == 53) {
        int days = ((year % 4 == 0 && year % 100) || year % 400 == 0) ? 366 : 365;
        if (lt->tm_yday - wday + 3 >= days) { year++; week = 1; }   // Thursday falls next year
    }
    return year * 100 + week;
}

int32_t stats_key(int period, const struct tm *lt) {
    int32_t day = (lt->tm_year + 1900) * 1000 + lt->tm_yday;     // YYYYDDD
    switch (period) {
    case ST_HOUR:  return day * 100 + lt->tm_hour;               // fits int32 to year 21474
    case ST_DAY:   return day;
    case ST_WEEK:  return iso_week_key(lt);
    case ST_MONTH: return (lt->tm_year + 1900) * 100 + lt->tm_mon + 1;
    }
    return 0;
}

unsigned stats_roll(stats_t *s, const struct tm *lt, stats_t *closed) {
    unsigned mask = 0;
    if (closed) *closed = *s;
    for (int p = 0; p < ST_N; p++) {
        int32_t k = stats_key(p, lt);
        if (s->key[p] == k) continue;
        if (s->key[p] != 0) {
            mask |= 1u << p;
            s->entries[p] = 0;
            s->occ_s[p] = 0;
        }
        s->key[p] = k;
    }
    return mask;
}

void stats_add_entry(stats_t *s) {
    for (int p = 0; p < ST_N; p++) s->entries[p]++;
}

void stats_add_occ(stats_t *s, uint32_t secs) {
    for (int p = 0; p < ST_N; p++) s->occ_s[p] += secs;
}

void stats_reset(stats_t *s, unsigned mask, const struct tm *lt) {
    for (int p = 0; p < ST_N; p++) {
        if (!(mask & (1u << p))) continue;
        s->entries[p] = 0;
        s->occ_s[p] = 0;
        s->key[p] = lt ? stats_key(p, lt) : 0;
    }
}

const char *stats_period_name(int period) {
    static const char *n[ST_N] = { "hour", "day", "week", "month" };
    return period >= 0 && period < ST_N ? n[period] : "?";
}

// YYYYDDD → calendar date (noon avoids any DST edge in mktime).
static struct tm day_tm(int32_t day) {
    struct tm t = { .tm_year = day / 1000 - 1900, .tm_mday = day % 1000 + 1,
                    .tm_hour = 12, .tm_isdst = -1 };
    mktime(&t);
    return t;
}

void stats_label(int period, int32_t key, char *buf, size_t n) {
    struct tm t;
    switch (period) {
    case ST_HOUR:
        t = day_tm(key / 100);
        snprintf(buf, n, "%04d-%02d-%02dT%02d", t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday, (int)(key % 100));
        break;
    case ST_DAY:
        t = day_tm(key);
        snprintf(buf, n, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        break;
    case ST_WEEK:  snprintf(buf, n, "%04d-W%02d", (int)(key / 100), (int)(key % 100)); break;
    case ST_MONTH: snprintf(buf, n, "%04d-%02d", (int)(key / 100), (int)(key % 100)); break;
    default:       snprintf(buf, n, "?");
    }
}
