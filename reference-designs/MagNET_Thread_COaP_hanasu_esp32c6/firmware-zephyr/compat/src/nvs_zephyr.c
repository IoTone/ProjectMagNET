/*
 * nvs_zephyr.c — IDF NVS API over Zephyr settings. Keys: "mn/<ns>/<key>".
 * OpenThread keeps its own dataset in the same settings store (under "ot"),
 * so FACTORY RESET — which erases namespaces, as on IDF — leaves Thread alone.
 */
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <stdio.h>

#include "nvs.h"
#include "nvs_flash.h"

#define NS_MAX   16                          /* IDF: 15 chars + NUL */
#define PATH_MAX_LEN (3 + NS_MAX + 1 + NS_MAX)

struct mn_nvs {
    char ns[NS_MAX];
    bool rw;
};

static bool s_settings_up;

esp_err_t nvs_flash_init(void) {
    if (s_settings_up) return ESP_OK;
    if (settings_subsys_init() != 0) return ESP_FAIL;
    s_settings_up = true;
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void) {
    return ESP_FAIL;                         /* never needed: settings self-heals */
}

static void mkpath(char *out, const struct mn_nvs *h, const char *key) {
    snprintf(out, PATH_MAX_LEN, "mn/%s/%s", h->ns, key);
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {
    if (!ns || strlen(ns) >= NS_MAX) return ESP_ERR_INVALID_ARG;
    if (nvs_flash_init() != ESP_OK) return ESP_FAIL;
    struct mn_nvs *h = k_malloc(sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;
    strcpy(h->ns, ns);
    h->rw = (mode == NVS_READWRITE);
    *out = h;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) { k_free(h); }
esp_err_t nvs_commit(nvs_handle_t h) { ARG_UNUSED(h); return ESP_OK; }

/* ---- read one exact key into a caller buffer ---- */
struct rd_ctx {
    void  *out;
    size_t cap;
    size_t len;                              /* stored length */
    bool   found;
    int    rc;
};

static int rd_cb(const char *key, size_t len, settings_read_cb read_cb,
                 void *cb_arg, void *param) {
    struct rd_ctx *c = param;
    if (key != NULL) return 0;               /* a deeper key, not this one */
    c->found = true;
    c->len = len;
    if (c->out && len <= c->cap) {
        ssize_t n = read_cb(cb_arg, c->out, len);
        c->rc = (n == (ssize_t)len) ? 0 : -1;
    }
    return 1;                                /* stop: exact match found */
}

static esp_err_t get_raw(nvs_handle_t h, const char *key, void *out, size_t cap,
                         size_t *stored) {
    char path[PATH_MAX_LEN];
    mkpath(path, h, key);
    struct rd_ctx c = { .out = out, .cap = cap };
    settings_load_subtree_direct(path, rd_cb, &c);
    if (!c.found || c.len == 0) return ESP_ERR_NVS_NOT_FOUND;  /* 0 = deleted */
    *stored = c.len;
    if (out && c.len > cap) return ESP_ERR_NVS_INVALID_LENGTH;
    return (c.rc == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t set_raw(nvs_handle_t h, const char *key, const void *val, size_t len) {
    if (!h->rw) return ESP_ERR_INVALID_STATE;
    char path[PATH_MAX_LEN];
    mkpath(path, h, key);
    return settings_save_one(path, val, len) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *val) {
    return set_raw(h, key, val, strlen(val) + 1);
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) {
    size_t stored = 0;
    esp_err_t e = get_raw(h, key, out, out ? *len : 0, &stored);
    if (e == ESP_OK || e == ESP_ERR_NVS_INVALID_LENGTH) *len = stored;
    return e;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len) {
    return set_raw(h, key, val, len);
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
    size_t stored = 0;
    esp_err_t e = get_raw(h, key, out, out ? *len : 0, &stored);
    if (e == ESP_OK || e == ESP_ERR_NVS_INVALID_LENGTH) *len = stored;
    return e;
}

/* Fixed-width ints: IDF rejects a size mismatch; so do we (as not-found). */
#define NVS_INT(sfx, T)                                                         \
    esp_err_t nvs_set_##sfx(nvs_handle_t h, const char *key, T v) {             \
        return set_raw(h, key, &v, sizeof(v));                                  \
    }                                                                           \
    esp_err_t nvs_get_##sfx(nvs_handle_t h, const char *key, T *v) {            \
        T tmp; size_t stored = 0;                                               \
        esp_err_t e = get_raw(h, key, &tmp, sizeof(tmp), &stored);              \
        if (e != ESP_OK) return e;                                              \
        if (stored != sizeof(tmp)) return ESP_ERR_NVS_NOT_FOUND;                \
        *v = tmp;                                                               \
        return ESP_OK;                                                          \
    }
NVS_INT(u8, uint8_t)
NVS_INT(u32, uint32_t)

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    if (!h->rw) return ESP_ERR_INVALID_STATE;
    char path[PATH_MAX_LEN];
    mkpath(path, h, key);
    return settings_delete(path) == 0 ? ESP_OK : ESP_FAIL;
}

/* ---- erase every key in the namespace: collect first, delete after ---- */
#define ERASE_MAX 48

struct ls_ctx {
    char names[ERASE_MAX][NS_MAX];
    int  n;
};

static int ls_cb(const char *key, size_t len, settings_read_cb read_cb,
                 void *cb_arg, void *param) {
    ARG_UNUSED(read_cb); ARG_UNUSED(cb_arg);
    struct ls_ctx *c = param;
    if (!key || len == 0 || c->n >= ERASE_MAX) return 0;
    strncpy(c->names[c->n], key, NS_MAX - 1);
    c->names[c->n][NS_MAX - 1] = '\0';
    c->n++;
    return 0;
}

esp_err_t nvs_erase_all(nvs_handle_t h) {
    if (!h->rw) return ESP_ERR_INVALID_STATE;
    char sub[PATH_MAX_LEN];
    snprintf(sub, sizeof(sub), "mn/%s", h->ns);
    struct ls_ctx *c = k_calloc(1, sizeof(*c));
    if (!c) return ESP_ERR_NO_MEM;
    settings_load_subtree_direct(sub, ls_cb, c);
    esp_err_t rc = ESP_OK;
    for (int i = 0; i < c->n; i++) {
        if (nvs_erase_key(h, c->names[i]) != ESP_OK) rc = ESP_FAIL;
    }
    k_free(c);
    return rc;
}

/* ---- iteration: snapshot the namespace's key list at find() time ---- */
struct mn_nvs_iter {
    struct ls_ctx keys;
    int           pos;
    nvs_type_t    type;
    char          ns[NS_MAX];
};

esp_err_t nvs_entry_find(const char *part, const char *ns, nvs_type_t type, nvs_iterator_t *it) {
    ARG_UNUSED(part);
    *it = NULL;
    if (!ns || strlen(ns) >= NS_MAX) return ESP_ERR_INVALID_ARG;
    if (nvs_flash_init() != ESP_OK) return ESP_FAIL;
    struct mn_nvs_iter *i = k_calloc(1, sizeof(*i));
    if (!i) return ESP_ERR_NO_MEM;
    char sub[PATH_MAX_LEN];
    snprintf(sub, sizeof(sub), "mn/%s", ns);
    settings_load_subtree_direct(sub, ls_cb, &i->keys);
    if (i->keys.n == 0) { k_free(i); return ESP_ERR_NVS_NOT_FOUND; }
    strcpy(i->ns, ns);
    i->type = type;
    *it = i;
    return ESP_OK;
}

esp_err_t nvs_entry_next(nvs_iterator_t *it) {
    if (!it || !*it) return ESP_ERR_INVALID_ARG;
    if (++(*it)->pos >= (*it)->keys.n) {
        k_free(*it);
        *it = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t nvs_entry_info(const nvs_iterator_t it, nvs_entry_info_t *out) {
    if (!it || !out) return ESP_ERR_INVALID_ARG;
    strncpy(out->namespace_name, it->ns, sizeof(out->namespace_name) - 1);
    out->namespace_name[sizeof(out->namespace_name) - 1] = '\0';
    strncpy(out->key, it->keys.names[it->pos], sizeof(out->key) - 1);
    out->key[sizeof(out->key) - 1] = '\0';
    out->type = it->type;
    return ESP_OK;
}

void nvs_release_iterator(nvs_iterator_t it) { k_free(it); }
