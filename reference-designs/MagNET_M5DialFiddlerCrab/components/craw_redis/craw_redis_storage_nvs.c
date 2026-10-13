/*
 * craw_redis_storage_nvs — NVS-backed storage for the v1 Capsule build.
 *
 * Layout (single namespace "redisdb"):
 *   string keys: NVS key = 's' + user_key  (≤ 15 chars total)
 *                value   = nvs_set_blob raw bytes
 *   list keys:   NVS key = 'l' + user_key
 *                value   = packed entries:
 *                          [u16 count][per entry: u16 len][len bytes]...
 *
 * The 's' / 'l' prefix lets KEYS / DBSIZE iterate the namespace and tell
 * the two value kinds apart while sharing the same user-key namespace —
 * SET foo and LPUSH foo collide as a Redis user would expect (WRONGTYPE).
 */

#include "craw_redis.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "redis_nvs";
static const char *NS  = "redisdb";

static int prefix_key(char prefix, const char *user, char out[17]) {
    if (!user) return -1;
    size_t ulen = strlen(user);
    if (ulen == 0 || ulen > CRAW_REDIS_KEY_MAX) return -1;
    out[0] = prefix;
    memcpy(out + 1, user, ulen);
    out[ulen + 1] = '\0';
    return 0;
}

/* ---- Strings ---- */

static int str_get(const char *k, char *out, size_t *out_len, size_t max) {
    char nk[17];
    if (prefix_key('s', k, nk) != 0) return -1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t sz = max;
    esp_err_t err = nvs_get_blob(h, nk, out, &sz);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return 1;
    if (err != ESP_OK) return -1;
    *out_len = sz;
    return 0;
}

static int str_set(const char *k, const char *v, size_t len) {
    char nk[17];
    if (prefix_key('s', k, nk) != 0) return -1;
    if (len > CRAW_REDIS_VALUE_MAX) return -2;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    /* SET overrides type: drop any list at the same user key. */
    char lk[17];
    prefix_key('l', k, lk);
    nvs_erase_key(h, lk);  /* ignore not-found */
    esp_err_t err = nvs_set_blob(h, nk, v, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK) ? 0 : -1;
}

static int key_del(const char *k) {
    char nk[17];
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    int hits = 0;
    if (prefix_key('s', k, nk) == 0 && nvs_erase_key(h, nk) == ESP_OK) hits++;
    if (prefix_key('l', k, nk) == 0 && nvs_erase_key(h, nk) == ESP_OK) hits++;
    if (hits) nvs_commit(h);
    nvs_close(h);
    return hits;
}

static int key_exists(const char *k) {
    char nk[17];
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    int kind = 0;
    size_t sz = 0;
    if (prefix_key('s', k, nk) == 0 &&
        nvs_get_blob(h, nk, NULL, &sz) == ESP_OK) kind = 1;
    else if (prefix_key('l', k, nk) == 0 &&
             nvs_get_blob(h, nk, NULL, &sz) == ESP_OK) kind = 2;
    nvs_close(h);
    return kind;
}

static int flush_all(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK) ? 0 : -1;
}

/* ---- Iteration ---- */

struct craw_redis_iter_s {
    nvs_iterator_t inner;
};

static craw_redis_iter_t *iter_open(void) {
    craw_redis_iter_t *it = calloc(1, sizeof(*it));
    if (!it) return NULL;
    /* ESP-IDF 5.x: nvs_entry_find returns esp_err_t, sets out param. */
    esp_err_t err = nvs_entry_find("nvs", NS, NVS_TYPE_BLOB, &it->inner);
    if (err != ESP_OK) {
        /* Empty namespace or NVS error — return an iterator that yields 0. */
        it->inner = NULL;
    }
    return it;
}

static int iter_next(craw_redis_iter_t *it, char *key_out, size_t max) {
    while (it && it->inner) {
        nvs_entry_info_t info;
        nvs_entry_info(it->inner, &info);
        /* Advance for the next call. */
        nvs_iterator_t next_it = it->inner;
        esp_err_t err = nvs_entry_next(&next_it);
        it->inner = (err == ESP_OK) ? next_it : NULL;
        /* Strip the 's' / 'l' prefix; skip anything else. */
        if (info.key[0] == 's' || info.key[0] == 'l') {
            size_t ulen = strlen(info.key + 1);
            if (ulen + 1 > max) continue;
            memcpy(key_out, info.key + 1, ulen + 1);
            return 1;
        }
    }
    return 0;
}

static void iter_close(craw_redis_iter_t *it) {
    if (!it) return;
    if (it->inner) nvs_release_iterator(it->inner);
    free(it);
}

static int dbsize(void) {
    /* DBSIZE counts unique user-keys. SET + LPUSH on the same key would
     * count as 2 NVS entries but 1 user key, so dedupe. */
    craw_redis_iter_t *it = iter_open();
    if (!it) return 0;
    int n = 0;
    char k[CRAW_REDIS_KEY_MAX + 1];
    char prev[CRAW_REDIS_KEY_MAX + 1] = "";
    while (iter_next(it, k, sizeof(k)) == 1) {
        if (strcmp(k, prev) != 0) {
            n++;
            strncpy(prev, k, sizeof(prev) - 1);
            prev[sizeof(prev) - 1] = '\0';
        }
    }
    iter_close(it);
    return n;
}

/* ---- Lists ----
 *
 * On-disk layout: [u16 count] [u16 len][bytes]... repeated.
 * All operations read the full blob, mutate, and write back. Cap is
 * CRAW_REDIS_LIST_TOTAL_MAX so write amplification stays bounded.
 */

static int list_load(const char *user_key, uint8_t *buf, size_t *len_out) {
    char nk[17];
    if (prefix_key('l', user_key, nk) != 0) return -1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 1;  /* not found */
    size_t sz = CRAW_REDIS_LIST_TOTAL_MAX;
    esp_err_t err = nvs_get_blob(h, nk, buf, &sz);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *len_out = 2;
        buf[0] = 0; buf[1] = 0;  /* count=0 */
        return 1;
    }
    if (err != ESP_OK) return -1;
    *len_out = sz;
    return 0;
}

static int list_save(const char *user_key, const uint8_t *buf, size_t len) {
    char nk[17];
    if (prefix_key('l', user_key, nk) != 0) return -1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    /* Saving a list overrides any string at the same key. */
    char sk[17];
    prefix_key('s', user_key, sk);
    nvs_erase_key(h, sk);
    esp_err_t err = nvs_set_blob(h, nk, buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK) ? 0 : -1;
}

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void     wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }

/* Walk to the i-th entry. Returns offset into buf, or -1 if out of range. */
static int list_offset(const uint8_t *buf, size_t buf_len, int idx,
                       uint16_t *entry_len_out) {
    uint16_t count = rd16(buf);
    if (idx < 0) idx += count;
    if (idx < 0 || idx >= count) return -1;
    size_t off = 2;
    int i = 0;
    while (i < idx) {
        if (off + 2 > buf_len) return -1;
        uint16_t l = rd16(buf + off);
        off += 2 + l;
        i++;
    }
    if (off + 2 > buf_len) return -1;
    if (entry_len_out) *entry_len_out = rd16(buf + off);
    return (int)off;
}

static int list_lpush(const char *k, const char *v, size_t vlen, int *new_len) {
    if (vlen > CRAW_REDIS_LIST_ENTRY_MAX) return -2;
    uint8_t buf[CRAW_REDIS_LIST_TOTAL_MAX];
    size_t len = 0;
    int rc = list_load(k, buf, &len);
    if (rc < 0) return -1;
    uint16_t count = rd16(buf);
    if (count >= CRAW_REDIS_LIST_ENTRIES_MAX) return -2;
    if (len + 2 + vlen > CRAW_REDIS_LIST_TOTAL_MAX) return -2;
    /* Shift existing entries right by (2 + vlen). */
    memmove(buf + 2 + 2 + vlen, buf + 2, len - 2);
    wr16(buf + 2, vlen);
    memcpy(buf + 4, v, vlen);
    wr16(buf, count + 1);
    if (new_len) *new_len = count + 1;
    return list_save(k, buf, len + 2 + vlen);
}

static int list_rpush(const char *k, const char *v, size_t vlen, int *new_len) {
    if (vlen > CRAW_REDIS_LIST_ENTRY_MAX) return -2;
    uint8_t buf[CRAW_REDIS_LIST_TOTAL_MAX];
    size_t len = 0;
    int rc = list_load(k, buf, &len);
    if (rc < 0) return -1;
    uint16_t count = rd16(buf);
    if (count >= CRAW_REDIS_LIST_ENTRIES_MAX) return -2;
    if (len + 2 + vlen > CRAW_REDIS_LIST_TOTAL_MAX) return -2;
    wr16(buf + len, vlen);
    memcpy(buf + len + 2, v, vlen);
    wr16(buf, count + 1);
    if (new_len) *new_len = count + 1;
    return list_save(k, buf, len + 2 + vlen);
}

static int list_pop(const char *k, char *out, size_t *out_len, size_t max,
                    bool from_head) {
    uint8_t buf[CRAW_REDIS_LIST_TOTAL_MAX];
    size_t len = 0;
    int rc = list_load(k, buf, &len);
    if (rc != 0) return 1;  /* not found / empty */
    uint16_t count = rd16(buf);
    if (count == 0) return 1;
    int idx = from_head ? 0 : (count - 1);
    uint16_t entry_len = 0;
    int off = list_offset(buf, len, idx, &entry_len);
    if (off < 0) return -1;
    if (entry_len > max) return -1;
    memcpy(out, buf + off + 2, entry_len);
    *out_len = entry_len;
    /* Splice out [off, off+2+entry_len). */
    size_t end = off + 2 + entry_len;
    memmove(buf + off, buf + end, len - end);
    len -= 2 + entry_len;
    wr16(buf, count - 1);
    if (count - 1 == 0) {
        /* Empty list — delete the key entirely so EXISTS returns 0. */
        char nk[17];
        prefix_key('l', k, nk);
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, nk);
            nvs_commit(h);
            nvs_close(h);
        }
        return 0;
    }
    return list_save(k, buf, len);
}

static int list_lpop(const char *k, char *out, size_t *out_len, size_t max) {
    return list_pop(k, out, out_len, max, true);
}
static int list_rpop(const char *k, char *out, size_t *out_len, size_t max) {
    return list_pop(k, out, out_len, max, false);
}

static int list_llen(const char *k, int *out) {
    uint8_t buf[CRAW_REDIS_LIST_TOTAL_MAX];
    size_t len = 0;
    int rc = list_load(k, buf, &len);
    if (rc < 0) return -1;
    *out = rc == 1 ? 0 : rd16(buf);
    return 0;
}

static int list_lindex(const char *k, int idx, char *out, size_t *out_len, size_t max) {
    uint8_t buf[CRAW_REDIS_LIST_TOTAL_MAX];
    size_t len = 0;
    int rc = list_load(k, buf, &len);
    if (rc != 0) return 1;
    uint16_t entry_len = 0;
    int off = list_offset(buf, len, idx, &entry_len);
    if (off < 0) return 1;
    if (entry_len > max) return -1;
    memcpy(out, buf + off + 2, entry_len);
    *out_len = entry_len;
    return 0;
}

static int list_lrange(const char *k, int start, int stop,
                       int (*emit)(const char *v, size_t len, void *ctx),
                       void *ctx) {
    uint8_t buf[CRAW_REDIS_LIST_TOTAL_MAX];
    size_t len = 0;
    int rc = list_load(k, buf, &len);
    if (rc < 0) return -1;
    uint16_t count = (rc == 1) ? 0 : rd16(buf);
    if (count == 0) return 0;
    if (start < 0) start += count;
    if (stop  < 0) stop  += count;
    if (start < 0) start = 0;
    if (stop >= count) stop = count - 1;
    if (start > stop) return 0;
    /* Walk in order, emitting indices [start, stop]. */
    size_t off = 2;
    for (int i = 0; i < count; i++) {
        if (off + 2 > len) break;
        uint16_t l = rd16(buf + off);
        if (i >= start && i <= stop) {
            int er = emit((const char *)(buf + off + 2), l, ctx);
            if (er != 0) return er;
        }
        off += 2 + l;
    }
    return 0;
}

/* ---- Vtable ---- */

static craw_redis_storage_t S_NVS = {
    .str_get      = str_get,
    .str_set      = str_set,
    .key_del      = key_del,
    .key_exists   = key_exists,
    .flush_all    = flush_all,
    .iter_open    = iter_open,
    .iter_next    = iter_next,
    .iter_close   = iter_close,
    .dbsize       = dbsize,
    .list_lpush   = list_lpush,
    .list_rpush   = list_rpush,
    .list_lpop    = list_lpop,
    .list_rpop    = list_rpop,
    .list_llen    = list_llen,
    .list_lindex  = list_lindex,
    .list_lrange  = list_lrange,
};

craw_redis_storage_t *craw_redis_storage_nvs(void) {
    /* nvs_flash_init() should already have been called by the app. */
    ESP_LOGI(TAG, "NVS storage ready (ns=%s, key max=%d, value max=%d)",
             NS, CRAW_REDIS_KEY_MAX, CRAW_REDIS_VALUE_MAX);
    return &S_NVS;
}
