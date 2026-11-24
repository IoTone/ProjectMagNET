// activity — see activity.h.

#include "activity.h"
#include <string.h>

void hist_init(hist_t *h) {
    memset(h, 0, sizeof(*h));
    h->magic = HIST_MAGIC;
}

bool hist_advance(hist_t *h, int32_t key_now) {
    if (h->key == key_now) return false;
    if (h->key == 0 || key_now < h->key) {      // first sync, or the clock stepped back
        h->key = key_now;
        return true;
    }
    int32_t n = key_now - h->key;
    if (n >= HIST_BINS) {
        memset(h->occ_s, 0, sizeof(h->occ_s));
    } else {
        memmove(h->occ_s, h->occ_s + n, (HIST_BINS - n) * sizeof(h->occ_s[0]));
        memset(h->occ_s + HIST_BINS - n, 0, n * sizeof(h->occ_s[0]));
    }
    h->key = key_now;
    return true;
}

void hist_add(hist_t *h, uint32_t secs) {
    uint32_t v = h->occ_s[HIST_BINS - 1] + secs;
    h->occ_s[HIST_BINS - 1] = v > HIST_BIN_S ? HIST_BIN_S : (uint16_t)v;
}

void recent_init(recent_t *r, int64_t now_ms) {
    memset(r, 0, sizeof(*r));
    r->slot = now_ms / RECENT_BIN_MS;
}

void recent_advance(recent_t *r, int64_t now_ms) {
    int64_t slot = now_ms / RECENT_BIN_MS;
    int64_t n = slot - r->slot;
    if (n <= 0) return;
    if (n >= RECENT_BINS) {
        memset(r->active_ms, 0, sizeof(r->active_ms));
    } else {
        memmove(r->active_ms, r->active_ms + n, (RECENT_BINS - n) * sizeof(r->active_ms[0]));
        memset(r->active_ms + RECENT_BINS - n, 0, n * sizeof(r->active_ms[0]));
    }
    r->slot = slot;
}

void recent_add(recent_t *r, int64_t now_ms, uint32_t active_ms) {
    recent_advance(r, now_ms);
    uint32_t v = r->active_ms[RECENT_BINS - 1] + active_ms;
    r->active_ms[RECENT_BINS - 1] = v > RECENT_BIN_MS ? RECENT_BIN_MS : (uint16_t)v;
}

uint32_t recent_total_ms(const recent_t *r) {
    uint32_t t = 0;
    for (int i = 0; i < RECENT_BINS; i++) t += r->active_ms[i];
    return t;
}
