/*
 * craw_role_bundle — install signed Forth role bundles delivered over the
 * MagNET hive. See ../docs/MagNET-RoleBundle-v1.md for the wire format.
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include "craw_role_bundle.h"
#include "keys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"

#include "forth_core.h"
#include "magnet_crypto.h"
#include "craw_hive.h"

static const char *TAG = "craw_role_bundle";

#define MAX_PROTO_VERSION   1
#define MAX_SRC_LEN         4096
#define NVS_NS              "role_bundle"
#define KEY_PREFIX_VERSION  "n:"  /* n:<name> -> version string */
#define KEY_PREFIX_BUNDLE   "b:"  /* b:<name> -> full envelope JSON */

/* ---------- Small helpers ---------- */

static int hex2bin(const char *hex, uint8_t *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        char hi = hex[i * 2], lo = hex[i * 2 + 1];
        if (!hi || !lo) return -1;
        int h = (hi >= '0' && hi <= '9') ? hi - '0'
              : (hi >= 'a' && hi <= 'f') ? 10 + hi - 'a'
              : (hi >= 'A' && hi <= 'F') ? 10 + hi - 'A' : -1;
        int l = (lo >= '0' && lo <= '9') ? lo - '0'
              : (lo >= 'a' && lo <= 'f') ? 10 + lo - 'a'
              : (lo >= 'A' && lo <= 'F') ? 10 + lo - 'A' : -1;
        if (h < 0 || l < 0) return -1;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return 0;
}

/* Parse "MAJOR.MINOR.PATCH" into 3 integers. Returns 0 on success.
 * Anything missing past major counts as 0. */
static int parse_semver(const char *s, int *maj, int *min, int *pat) {
    if (!s) return -1;
    *maj = *min = *pat = 0;
    int field = 0;
    int *out[3] = { maj, min, pat };
    int v = 0;
    int saw_digit = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            saw_digit = 1;
        } else if (*p == '.' || *p == '\0') {
            if (!saw_digit) return -1;
            *out[field] = v;
            v = 0;
            saw_digit = 0;
            field++;
            if (*p == '\0' || field >= 3) break;
        } else {
            return -1;  /* extra char (suffix etc.) — reject for v1 strict parse */
        }
    }
    return 0;
}

/* Compare two semvers. Returns <0 if a<b, 0 if equal, >0 if a>b. */
static int semver_cmp(const char *a, const char *b) {
    int amaj, amin, apat, bmaj, bmin, bpat;
    if (parse_semver(a, &amaj, &amin, &apat) != 0) return 0;
    if (parse_semver(b, &bmaj, &bmin, &bpat) != 0) return 0;
    if (amaj != bmaj) return amaj - bmaj;
    if (amin != bmin) return amin - bmin;
    return apat - bpat;
}

/* ---------- Trust store lookup ---------- */

/* Match on (author, alg): an author may hold one entry per algorithm, which
 * is what lets a node accept v1-HMAC and v2-Ed25519 bundles side by side
 * during migration. */
static const craw_role_bundle_trust_entry_t *lookup_author(const char *author, int alg) {
    for (size_t i = 0; i < CRAW_ROLE_BUNDLE_TRUST_COUNT; i++) {
        if (CRAW_ROLE_BUNDLE_TRUST_STORE[i].alg == alg &&
            strcmp(CRAW_ROLE_BUNDLE_TRUST_STORE[i].author, author) == 0)
            return &CRAW_ROLE_BUNDLE_TRUST_STORE[i];
    }
    return NULL;
}

/* ---------- Canonical signing input ---------- */

int craw_role_bundle_signing_input(const char *name, const char *version,
                                   int min_proto, const char *author,
                                   const char *crc32_hex, const char *src_b64,
                                   char *buf, size_t buf_len) {
    if (!name || !version || !author || !crc32_hex || !src_b64) return -1;
    int n = snprintf(buf, buf_len, "%s|%s|%d|%s|%s|%s",
                     name, version, min_proto, author, crc32_hex, src_b64);
    if (n < 0 || (size_t)n >= buf_len) return -1;
    return n;
}

/* ---------- Signature verification ---------- */

static int verify_hmac_sha256(const uint8_t *key, size_t key_len,
                              const char *to_sign,
                              const char *sig_hex) {
    uint8_t expected[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    if (mbedtls_md_hmac(info, key, key_len,
                        (const uint8_t *)to_sign, strlen(to_sign),
                        expected) != 0) return -1;

    uint8_t got[32];
    if (strlen(sig_hex) != 64) return -1;
    if (hex2bin(sig_hex, got, 32) != 0) return -1;

    /* Constant-time compare. */
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (expected[i] ^ got[i]);
    return diff == 0 ? 0 : -1;
}

/* ---------- caps_req ⊂ node_caps ---------- */

static bool caps_covered(const cJSON *caps_req,
                         const char **node_caps, int n_caps) {
    if (!caps_req || !cJSON_IsArray(caps_req)) return true;
    cJSON *req;
    cJSON_ArrayForEach(req, caps_req) {
        if (!cJSON_IsString(req)) return false;
        bool found = false;
        for (int i = 0; i < n_caps; i++) {
            if (node_caps[i] && strcmp(node_caps[i], req->valuestring) == 0) {
                found = true; break;
            }
        }
        if (!found) return false;
    }
    return true;
}

/* ---------- NVS helpers ---------- */

static int nvs_save_pair(const char *name, const char *version, const char *envelope_json) {
    if (strlen(name) > 30) return -1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    char key[40];
    snprintf(key, sizeof(key), KEY_PREFIX_VERSION "%s", name);
    esp_err_t e1 = nvs_set_str(h, key, version);
    snprintf(key, sizeof(key), KEY_PREFIX_BUNDLE "%s", name);
    esp_err_t e2 = nvs_set_str(h, key, envelope_json);
    esp_err_t e3 = nvs_commit(h);
    nvs_close(h);
    return (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK) ? 0 : -1;
}

static int nvs_load_version(const char *name, char *out, size_t out_len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    char key[40];
    snprintf(key, sizeof(key), KEY_PREFIX_VERSION "%s", name);
    size_t sz = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &sz);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

/* ---------- Hive KV words for role bundles (punch-list H8 enabler) ----------
 *
 * The per-node kv-get / kv-put REPL words are interactive — they prompt for
 * the key on the console, which would hang a role-tick forever. These are
 * the stack-based versions role bundles actually need (strings come from
 * s" — addr/len pairs):
 *
 *   hkv-put$  ( v-addr v-len k-addr k-len -- )       fire-and-forget KV_PUT
 *   hkv-get$  ( k-addr k-len -- v-addr v-len -1 | 0 ) value in a static buf
 *   hkv-run   ( k-addr k-len -- )  fetch; if non-empty: CLEAR the key, then
 *                                  forth_eval the value. The generalized
 *                                  boombox:cmd pattern — hive commands are
 *                                  Forth phrases. Clear-before-eval gives
 *                                  at-most-once execution.
 *
 * Static buffers are safe: FFI words only ever run under the engine lock
 * (ESPIDFORTH >= 0.5.0), and hkv-run's nested forth_eval is why that lock
 * is recursive. The 2 s kv-get timeout bounds how long a tick can block. */

/* Role-word values are short (commands, status strings) — capping the value
 * buffer well below CRAW_HIVE_KV_VALUE_MAX keeps ~3 KB of BSS off DRAM-tight
 * hosts (the classic-ESP32 camera overflowed dram0 by 2.2 KB with the full
 * buffer). Longer values are truncated safely by the kv layer; bundles that
 * need bulk data should fetch it through the C side. */
#define HKV_VAL_MAX 256
static char s_hkv_key[CRAW_HIVE_KV_KEY_MAX + 1];
static char s_hkv_val[HKV_VAL_MAX + 1];

static void pop_str(char *out, size_t cap) {
    intptr_t len  = forth_pop();
    intptr_t addr = forth_pop();
    size_t n = (addr && len > 0) ? (size_t)len : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(out, (const void *)addr, n);
    out[n] = '\0';
}

static void w_hkv_put(void) {
    pop_str(s_hkv_key, sizeof(s_hkv_key));
    pop_str(s_hkv_val, sizeof(s_hkv_val));
    craw_hive_node_kv_put(s_hkv_key, s_hkv_val);
}

static void w_hkv_get(void) {
    pop_str(s_hkv_key, sizeof(s_hkv_key));
    int rc = craw_hive_node_kv_get(s_hkv_key, s_hkv_val, sizeof(s_hkv_val), 2000);
    if (rc == 0) {
        forth_push((intptr_t)s_hkv_val);
        forth_push((intptr_t)strlen(s_hkv_val));
        forth_push(-1);
    } else {
        forth_push(0);
    }
}

static void w_hkv_run(void) {
    pop_str(s_hkv_key, sizeof(s_hkv_key));
    int rc = craw_hive_node_kv_get(s_hkv_key, s_hkv_val, sizeof(s_hkv_val), 2000);
    if (rc != 0 || !s_hkv_val[0]) return;
    /* Clear first (at-most-once), and eval a private copy — a nested hkv
     * word inside the command would clobber the static buffers. */
    char key[CRAW_HIVE_KV_KEY_MAX + 1];
    strcpy(key, s_hkv_key);
    craw_hive_node_kv_put(key, "");
    char *cmd = strdup(s_hkv_val);
    if (!cmd) return;
    forth_eval(cmd);
    free(cmd);
}

void craw_role_bundle_register_hive_words(void) {
    forth_register_word("hkv-put$", w_hkv_put);
    forth_register_word("hkv-get$", w_hkv_get);
    forth_register_word("hkv-run",  w_hkv_run);
}

/* ---------- Role lifecycle (punch-list H5) ----------
 *
 * Bundles cannot loop — the convention is four optional words and a
 * host-owned timer:
 *   role-init    once, at install; failure rolls the whole bundle back
 *   role-tick    called every tick_ms from a dedicated task; must return
 *   role-stop    called before this role is replaced (or on demand)
 *   role-status  on demand (REPL / ruler query) — pure convention, no code
 *
 * One active tick role per node (v1). The tick task is sequential, so
 * overruns cannot stack — a tick that runs past its period just delays the
 * next one and increments the overrun counter (Hanasu's circuit-breaker
 * lesson: bound the damage, count the evidence). Words are invoked by NAME
 * through forth_eval(), so redefinition and rollback are always respected;
 * the engine's internal lock (ESPIDFORTH >= 0.5.0) serializes us against
 * the REPL and the install worker. */

#define TICK_MS_DEFAULT   1000
#define TICK_MS_MIN        100
#define TICK_MS_MAX      60000
#define TICK_MIN_IDLE_MS    50   /* always yield this long, even on overrun */

static volatile bool s_tick_stop = false;
static TaskHandle_t  s_tick_task = NULL;
static int           s_tick_ms = 0;
static int           s_tick_overruns = 0;
static char          s_tick_role[33] = "";

static void role_tick_task(void *arg) {
    (void)arg;
    while (!s_tick_stop) {
        int64_t t0 = esp_timer_get_time();
        forth_eval("role-tick");
        int elapsed_ms = (int)((esp_timer_get_time() - t0) / 1000);
        int delay = s_tick_ms - elapsed_ms;
        if (delay < TICK_MIN_IDLE_MS) {
            s_tick_overruns++;
            delay = TICK_MIN_IDLE_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
    s_tick_task = NULL;
    vTaskDelete(NULL);
}

void craw_role_bundle_role_stop(void) {
    if (s_tick_task) {
        s_tick_stop = true;
        /* A conforming role-tick returns promptly; give a misbehaving one
         * two seconds before we give up waiting (the task will still exit
         * whenever its eval returns). */
        for (int i = 0; i < 100 && s_tick_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
        s_tick_stop = false;
        if (s_tick_task) ESP_LOGW(TAG, "role-tick task did not stop within 2s");
    }
    if (forth_word_exists("role-stop")) forth_eval("role-stop");
    s_tick_role[0] = '\0';
}

int craw_role_bundle_tick_overruns(void) { return s_tick_overruns; }
const char *craw_role_bundle_tick_role(void) {
    return s_tick_role[0] ? s_tick_role : NULL;
}

static void role_tick_start(const char *name, int tick_ms) {
    s_tick_ms = tick_ms;
    s_tick_stop = false;
    s_tick_overruns = 0;
    snprintf(s_tick_role, sizeof(s_tick_role), "%s", name);
    if (xTaskCreate(role_tick_task, "role_tick", 4096, NULL, 3, &s_tick_task) != pdPASS) {
        s_tick_task = NULL;
        s_tick_role[0] = '\0';
        ESP_LOGW(TAG, "role_tick task create failed — '%s' installed without ticker", name);
    } else {
        ESP_LOGI(TAG, "role '%s' ticking every %d ms", name, tick_ms);
    }
}

/* ---------- Public: install ---------- */

static void set_status(craw_role_bundle_install_result_t *r,
                       craw_role_bundle_err_t status, const char *field) {
    if (!r) return;
    r->status = status;
    if (field) {
        strncpy(r->err_field, field, sizeof(r->err_field) - 1);
        r->err_field[sizeof(r->err_field) - 1] = '\0';
    } else {
        r->err_field[0] = '\0';
    }
}

int craw_role_bundle_install_from_json(const char *json,
                                       const char **node_caps, int n_caps,
                                       craw_role_bundle_install_result_t *result) {
    if (result) { result->status = BUNDLE_OK; result->err_field[0] = '\0'; result->err_detail[0] = '\0'; }
    if (!json) { set_status(result, BUNDLE_ERR_PARSE, "json"); return BUNDLE_ERR_PARSE; }

    cJSON *env = cJSON_Parse(json);
    if (!env || !cJSON_IsObject(env)) {
        cJSON_Delete(env);
        set_status(result, BUNDLE_ERR_PARSE, "envelope");
        return BUNDLE_ERR_PARSE;
    }

#define GET_STR(field, var) \
    const cJSON *j_##var = cJSON_GetObjectItemCaseSensitive(env, field); \
    if (!cJSON_IsString(j_##var)) { \
        set_status(result, BUNDLE_ERR_PARSE, field); \
        cJSON_Delete(env); return BUNDLE_ERR_PARSE; \
    } \
    const char *var = j_##var->valuestring;

    GET_STR("name",     name);
    GET_STR("version",  version);
    GET_STR("author",   author);
    GET_STR("crc32",    crc32_hex);
    GET_STR("sig_alg",  sig_alg);
    GET_STR("sig",      sig_hex);
    GET_STR("src_b64",  src_b64);
#undef GET_STR

    const cJSON *j_min_proto = cJSON_GetObjectItemCaseSensitive(env, "min_proto");
    if (!cJSON_IsNumber(j_min_proto)) {
        set_status(result, BUNDLE_ERR_PARSE, "min_proto");
        cJSON_Delete(env); return BUNDLE_ERR_PARSE;
    }
    int min_proto = (int)j_min_proto->valuedouble;

    if (min_proto > MAX_PROTO_VERSION) {
        ESP_LOGW(TAG, "bundle '%s' wants min_proto=%d, we are %d", name, min_proto, MAX_PROTO_VERSION);
        set_status(result, BUNDLE_ERR_PROTO, "min_proto");
        cJSON_Delete(env); return BUNDLE_ERR_PROTO;
    }

    /* Algorithm dispatch, then trust lookup on (author, alg). */
    int want_alg;
    if      (strcmp(sig_alg, "hmac-sha256") == 0) want_alg = TRUST_ALG_HMAC_SHA256;
    else if (strcmp(sig_alg, "ed25519")     == 0) want_alg = TRUST_ALG_ED25519;
    else {
        ESP_LOGW(TAG, "unsupported sig_alg '%s'", sig_alg);
        set_status(result, BUNDLE_ERR_AUTHOR, "sig_alg");
        cJSON_Delete(env); return BUNDLE_ERR_AUTHOR;
    }

    const craw_role_bundle_trust_entry_t *trust = lookup_author(author, want_alg);
    if (!trust) {
        ESP_LOGW(TAG, "no trust entry for author '%s' with alg '%s'", author, sig_alg);
        set_status(result, BUNDLE_ERR_AUTHOR, "author");
        cJSON_Delete(env); return BUNDLE_ERR_AUTHOR;
    }

    /* Canonical signing input */
    char to_sign[6 * 1024];
    int n = craw_role_bundle_signing_input(name, version, min_proto, author,
                                           crc32_hex, src_b64,
                                           to_sign, sizeof(to_sign));
    if (n < 0) {
        set_status(result, BUNDLE_ERR_INTERNAL, "signing_input");
        cJSON_Delete(env); return BUNDLE_ERR_INTERNAL;
    }

    /* Signature check. Both algs sign the same canonical pipe-string; only
     * the primitive differs. HMAC proves hive membership; Ed25519 proves the
     * author (private key held offline) — the R10 upgrade. */
    bool sig_ok;
    if (want_alg == TRUST_ALG_ED25519) {
        uint8_t sig[64];
        sig_ok = strlen(sig_hex) == 128 &&
                 hex2bin(sig_hex, sig, sizeof(sig)) == 0 &&
                 magnet_ed25519_verify(trust->key, sig,
                                       (const uint8_t *)to_sign, strlen(to_sign));
    } else {
        sig_ok = verify_hmac_sha256(trust->key, trust->key_len, to_sign, sig_hex) == 0;
    }
    if (!sig_ok) {
        ESP_LOGW(TAG, "bundle '%s' signature mismatch (%s)", name, sig_alg);
        set_status(result, BUNDLE_ERR_SIG, "sig");
        cJSON_Delete(env); return BUNDLE_ERR_SIG;
    }

    /* Base64-decode source */
    size_t src_len = 0;
    size_t src_b64_len = strlen(src_b64);
    /* mbedtls_base64_decode wants destination buffer; size estimate is len*3/4 + 4. */
    uint8_t *src = malloc(MAX_SRC_LEN + 1);
    if (!src) {
        set_status(result, BUNDLE_ERR_INTERNAL, "alloc");
        cJSON_Delete(env); return BUNDLE_ERR_INTERNAL;
    }
    int b64rc = mbedtls_base64_decode(src, MAX_SRC_LEN, &src_len,
                                      (const uint8_t *)src_b64, src_b64_len);
    if (b64rc != 0) {
        ESP_LOGW(TAG, "base64 decode failed rc=-0x%x", -b64rc);
        free(src);
        set_status(result, BUNDLE_ERR_BASE64, "src_b64");
        cJSON_Delete(env); return BUNDLE_ERR_BASE64;
    }
    src[src_len] = '\0';

    /* CRC32 over decoded source */
    uint32_t want_crc = 0;
    if (sscanf(crc32_hex, "%" SCNx32, &want_crc) != 1) {
        free(src);
        set_status(result, BUNDLE_ERR_PARSE, "crc32");
        cJSON_Delete(env); return BUNDLE_ERR_PARSE;
    }
    /* esp_rom_crc32_le: standard CRC-32 (poly 0xEDB88320 reflected). Init UINT32_MAX, XOR-out UINT32_MAX. */
    uint32_t actual_crc = ~esp_rom_crc32_le(0xffffffff, src, src_len);
    if (actual_crc != want_crc) {
        ESP_LOGW(TAG, "crc32 mismatch: want 0x%08" PRIx32 " got 0x%08" PRIx32,
                 want_crc, actual_crc);
        free(src);
        set_status(result, BUNDLE_ERR_CRC, "crc32");
        cJSON_Delete(env); return BUNDLE_ERR_CRC;
    }

    /* Caps check */
    const cJSON *caps_req = cJSON_GetObjectItemCaseSensitive(env, "caps_req");
    if (!caps_covered(caps_req, node_caps, n_caps)) {
        ESP_LOGW(TAG, "bundle '%s' caps_req not covered by node", name);
        free(src);
        set_status(result, BUNDLE_ERR_CAPS, "caps_req");
        cJSON_Delete(env); return BUNDLE_ERR_CAPS;
    }

    /* Version monotonicity */
    char persisted[24];
    if (nvs_load_version(name, persisted, sizeof(persisted)) == 0) {
        if (semver_cmp(version, persisted) < 0) {
            ESP_LOGW(TAG, "refusing downgrade '%s' %s -> %s", name, persisted, version);
            free(src);
            set_status(result, BUNDLE_ERR_VERSION, "version");
            cJSON_Delete(env); return BUNDLE_ERR_VERSION;
        }
    }

    /* tick_ms: host-owned cadence for role-tick. Optional; like caps_req it
     * sits OUTSIDE the v1 canonical signing input (documented limitation). */
    int tick_ms = TICK_MS_DEFAULT;
    const cJSON *j_tick = cJSON_GetObjectItemCaseSensitive(env, "tick_ms");
    if (cJSON_IsNumber(j_tick)) {
        tick_ms = (int)j_tick->valuedouble;
        if (tick_ms < TICK_MS_MIN) tick_ms = TICK_MS_MIN;
        if (tick_ms > TICK_MS_MAX) tick_ms = TICK_MS_MAX;
    }

    /* Retire the incumbent role BEFORE the new source lands, so its own
     * role-stop (about to be shadowed or replaced) does the teardown. */
    craw_role_bundle_role_stop();

    /* Outer savepoint spans eval + role-init: if role-init fails, the whole
     * bundle disappears, not just the init call. */
    forth_savepoint_t sp0;
    forth_save(&sp0);

    /* Install through the engine's verified-apply primitive: savepoint →
     * line-at-a-time eval → rollback on first error, failing line reported.
     * (ESPIDFORTH ≥ 0.4.0; shared with the WaveC6LED OTA path — punch-list
     * H4. The engine works on its own copy, so src stays pristine.) */
    char fail_line[80] = "";
    int frc = forth_eval_rollback((const char *)src, src_len,
                                  fail_line, sizeof(fail_line));
    if (frc < 0) {
        ESP_LOGW(TAG, "bundle '%s' eval alloc failure", name);
        free(src);
        set_status(result, BUNDLE_ERR_INTERNAL, "eval_alloc");
        cJSON_Delete(env); return BUNDLE_ERR_INTERNAL;
    }
    if (frc != 0) {
        ESP_LOGW(TAG, "bundle '%s' install FAILED, rolled back — failed at: %s",
                 name, fail_line);
        free(src);
        set_status(result, BUNDLE_ERR_EVAL, "src");
        if (result) {
            snprintf(result->err_detail, sizeof(result->err_detail), "%s", fail_line);
        }
        cJSON_Delete(env); return BUNDLE_ERR_EVAL;
    }

    /* role-init: once, at install (H5 lifecycle). A failing init rolls the
     * WHOLE bundle back — a role that cannot set up must not half-exist. */
    if (forth_word_exists("role-init")) {
        int errs_before_init = forth_error_count();
        forth_eval("role-init");
        if (forth_error_count() != errs_before_init) {
            forth_restore(&sp0);
            ESP_LOGW(TAG, "bundle '%s' role-init FAILED — whole bundle rolled back", name);
            free(src);
            set_status(result, BUNDLE_ERR_EVAL, "role-init");
            if (result) {
                snprintf(result->err_detail, sizeof(result->err_detail), "role-init failed");
            }
            cJSON_Delete(env); return BUNDLE_ERR_EVAL;
        }
    }

    /* Persist envelope. Even if NVS fails, the install above is already
     * live in the Forth vocabulary — so report partial success. */
    int nrc = nvs_save_pair(name, version, json);
    if (nrc != 0) {
        ESP_LOGW(TAG, "NVS persist failed for bundle '%s' (still installed in RAM)", name);
        free(src);
        set_status(result, BUNDLE_ERR_NVS, "nvs");
        cJSON_Delete(env); return BUNDLE_ERR_NVS;
    }

    /* Start the ticker last — only for a bundle that fully installed,
     * persisted, and (if present) initialized. */
    if (forth_word_exists("role-tick")) role_tick_start(name, tick_ms);

    ESP_LOGI(TAG, "bundle '%s' v%s installed (src %u bytes)",
             name, version, (unsigned)src_len);

    if (result) {
        result->status = BUNDLE_OK;
        result->err_field[0] = '\0';
        strncpy(result->info.name,    name,    sizeof(result->info.name) - 1);
        strncpy(result->info.version, version, sizeof(result->info.version) - 1);
        strncpy(result->info.author,  author,  sizeof(result->info.author) - 1);
        result->info.min_proto = min_proto;
        result->info.crc32     = actual_crc;
        result->info.src_len   = src_len;
    }

    free(src);
    cJSON_Delete(env);
    return BUNDLE_OK;
}

/* ---------- Apply persisted bundles on boot ---------- */

int craw_role_bundle_apply_saved(const char **node_caps, int n_caps) {
    int applied = 0;
    nvs_iterator_t it = NULL;
    /* Iterate all "b:<name>" entries — each is a saved envelope. */
    if (nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NS, NVS_TYPE_STR, &it) != ESP_OK) {
        return 0;
    }
    while (it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, KEY_PREFIX_BUNDLE, 2) == 0) {
            /* Open + load + reinstall */
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
                size_t sz = 0;
                if (nvs_get_str(h, info.key, NULL, &sz) == ESP_OK && sz > 0 && sz < 8192) {
                    char *buf = malloc(sz);
                    if (buf) {
                        if (nvs_get_str(h, info.key, buf, &sz) == ESP_OK) {
                            int rc = craw_role_bundle_install_from_json(
                                buf, node_caps, n_caps, NULL);
                            if (rc == BUNDLE_OK) applied++;
                            else {
                                ESP_LOGW(TAG, "skipping persisted '%s' rc=%d",
                                         info.key + 2, rc);
                            }
                        }
                        free(buf);
                    }
                }
                nvs_close(h);
            }
        }
        if (nvs_entry_next(&it) != ESP_OK) break;
    }
    if (it) nvs_release_iterator(it);
    return applied;
}

int craw_role_bundle_forget(const char *name) {
    if (!name) return -1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    char key[40];
    snprintf(key, sizeof(key), KEY_PREFIX_VERSION "%s", name);
    nvs_erase_key(h, key);
    snprintf(key, sizeof(key), KEY_PREFIX_BUNDLE "%s", name);
    nvs_erase_key(h, key);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

int craw_role_bundle_forget_all(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_erase_all(h);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? 0 : -1;
}

int craw_role_bundle_iterate(craw_role_bundle_iter_cb_t cb, void *ctx) {
    int n = 0;
    nvs_iterator_t it = NULL;
    if (nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NS, NVS_TYPE_STR, &it) != ESP_OK) {
        return 0;
    }
    while (it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, KEY_PREFIX_VERSION, 2) == 0) {
            const char *name = info.key + 2;
            char ver[24] = {0};
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
                size_t sz = sizeof(ver);
                nvs_get_str(h, info.key, ver, &sz);
                nvs_close(h);
            }
            if (cb && cb(name, ver, ctx) != 0) {
                if (it) nvs_release_iterator(it);
                return n;
            }
            n++;
        }
        if (nvs_entry_next(&it) != ESP_OK) break;
    }
    if (it) nvs_release_iterator(it);
    return n;
}

int craw_role_bundle_format_applied_json(const craw_role_bundle_install_result_t *r,
                                         const char *role_fallback,
                                         char *buf, size_t buf_len) {
    if (!r || !buf || buf_len == 0) return -1;
    long long ts = (long long)time(NULL);
    int n;
    if (r->status == BUNDLE_OK) {
        n = snprintf(buf, buf_len,
                     "{\"name\":\"%s\",\"version\":\"%s\",\"ok\":true,\"ts\":%lld}",
                     r->info.name, r->info.version, ts);
    } else {
        /* Sanitize the failing line the same way the Wave OTA reporter does:
         * strip quotes/backslashes/control chars so a Forth diagnostic can
         * never break the JSON — the failure report is the one message we
         * most want to survive. */
        char at[sizeof(r->err_detail)];
        size_t j = 0;
        for (size_t i = 0; r->err_detail[i] && j < sizeof(at) - 1; i++) {
            char c = r->err_detail[i];
            if (c == '"' || c == '\\' || (unsigned char)c < 0x20) c = ' ';
            at[j++] = c;
        }
        at[j] = '\0';
        n = snprintf(buf, buf_len,
                     "{\"name\":\"%s\",\"ok\":false,\"err\":%d,\"field\":\"%s\",\"at\":\"%s\",\"ts\":%lld}",
                     role_fallback ? role_fallback : "?",
                     (int)r->status, r->err_field, at, ts);
    }
    return (n < 0 || (size_t)n >= buf_len) ? -1 : 0;
}

void craw_role_bundle_init(void) {
    /* No global state needed yet — kept for future use (warm-up trust
     * store, validate keys.h, etc.). */
}
