/*
 * craw_hive_proto — message framing + HMAC for the MagNET hive protocol.
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include "craw_hive.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"
#include "mbedtls/md.h"

static const char *TAG = "craw_hive_proto";

/* ---- Small hex helpers ---- */

static const char HEX_DIGITS[] = "0123456789abcdef";

static void bin2hex(const uint8_t *in, size_t in_len, char *out) {
    for (size_t i = 0; i < in_len; i++) {
        out[i * 2]     = HEX_DIGITS[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = HEX_DIGITS[in[i] & 0xF];
    }
    out[in_len * 2] = '\0';
}

static int hex2bin(const char *in, uint8_t *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        char hi = in[i * 2], lo = in[i * 2 + 1];
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

/* ---- Type names ---- */

const char *craw_hive_msg_type_name(craw_hive_msg_type_t t) {
    switch (t) {
        case CRAW_HIVE_MSG_HELLO:        return "HELLO";
        case CRAW_HIVE_MSG_WELCOME:      return "WELCOME";
        case CRAW_HIVE_MSG_REJECT:       return "REJECT";
        case CRAW_HIVE_MSG_PING:         return "PING";
        case CRAW_HIVE_MSG_ROLE_REQUEST: return "ROLE_REQUEST";
        case CRAW_HIVE_MSG_ROLE_GRANT:   return "ROLE_GRANT";
        case CRAW_HIVE_MSG_KV_GET:       return "KV_GET";
        case CRAW_HIVE_MSG_KV_DATA:      return "KV_DATA";
        case CRAW_HIVE_MSG_KV_PUT:       return "KV_PUT";
        case CRAW_HIVE_MSG_KV_NOT_FOUND: return "KV_NOT_FOUND";
        case CRAW_HIVE_MSG_CHALLENGE:    return "CHALLENGE";
        case CRAW_HIVE_MSG_RESPONSE:     return "RESPONSE";
        default:                         return "UNKNOWN";
    }
}

craw_hive_msg_type_t craw_hive_msg_type_parse(const char *s) {
    if (!s) return CRAW_HIVE_MSG_UNKNOWN;
    if (!strcmp(s, "HELLO"))        return CRAW_HIVE_MSG_HELLO;
    if (!strcmp(s, "WELCOME"))      return CRAW_HIVE_MSG_WELCOME;
    if (!strcmp(s, "REJECT"))       return CRAW_HIVE_MSG_REJECT;
    if (!strcmp(s, "PING"))         return CRAW_HIVE_MSG_PING;
    if (!strcmp(s, "ROLE_REQUEST")) return CRAW_HIVE_MSG_ROLE_REQUEST;
    if (!strcmp(s, "ROLE_GRANT"))   return CRAW_HIVE_MSG_ROLE_GRANT;
    if (!strcmp(s, "KV_GET"))       return CRAW_HIVE_MSG_KV_GET;
    if (!strcmp(s, "KV_DATA"))      return CRAW_HIVE_MSG_KV_DATA;
    if (!strcmp(s, "KV_PUT"))       return CRAW_HIVE_MSG_KV_PUT;
    if (!strcmp(s, "KV_NOT_FOUND")) return CRAW_HIVE_MSG_KV_NOT_FOUND;
    if (!strcmp(s, "CHALLENGE"))    return CRAW_HIVE_MSG_CHALLENGE;
    if (!strcmp(s, "RESPONSE"))     return CRAW_HIVE_MSG_RESPONSE;
    return CRAW_HIVE_MSG_UNKNOWN;
}

/* ---- Nonce ---- */

void craw_hive_nonce_fill(char *buf) {
    uint8_t raw[CRAW_HIVE_NONCE_BYTES];
    for (size_t i = 0; i < sizeof(raw); i += 4) {
        uint32_t r = esp_random();
        memcpy(raw + i, &r, (i + 4 <= sizeof(raw)) ? 4 : sizeof(raw) - i);
    }
    bin2hex(raw, sizeof(raw), buf);
}

/* ---- msg lifecycle ---- */

void craw_hive_msg_free(craw_hive_msg_t *msg) {
    if (!msg) return;
    free(msg->payload_json);
    msg->payload_json = NULL;
}

/* ---- Canonical payload printer ----
 * We cannot rely on cJSON to give us alphabetically-sorted keys, so we
 * walk the object and emit keys in sorted order. cJSON objects are
 * small in our protocol (<= ~20 keys), so an O(n^2) selection is fine.
 */
static char *canonicalize_object(const cJSON *obj);

static char *canonicalize_value(const cJSON *v) {
    if (!v) {
        char *s = strdup("null"); return s;
    }
    if (cJSON_IsNull(v))   return strdup("null");
    if (cJSON_IsTrue(v))   return strdup("true");
    if (cJSON_IsFalse(v))  return strdup("false");
    if (cJSON_IsNumber(v)) {
        char buf[32];
        double d = v->valuedouble;
        if (d == (double)(int64_t)d)
            snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)d);
        else
            snprintf(buf, sizeof(buf), "%.17g", d);
        return strdup(buf);
    }
    if (cJSON_IsString(v)) {
        /* Re-escape via cJSON for correctness */
        cJSON *wrap = cJSON_CreateString(v->valuestring);
        char *s = cJSON_PrintUnformatted(wrap);
        cJSON_Delete(wrap);
        return s;
    }
    if (cJSON_IsArray(v)) {
        cJSON *child;
        size_t total = 3; /* []\0 */
        char **parts = NULL;
        int n = 0;
        cJSON_ArrayForEach(child, v) {
            char *p = canonicalize_value(child);
            if (!p) goto oom_arr;
            parts = realloc(parts, sizeof(char*) * (n + 1));
            parts[n++] = p;
            total += strlen(p) + 1;
        }
        char *out = malloc(total);
        if (!out) goto oom_arr;
        char *w = out;
        *w++ = '[';
        for (int i = 0; i < n; i++) {
            if (i) *w++ = ',';
            size_t l = strlen(parts[i]);
            memcpy(w, parts[i], l);
            w += l;
            free(parts[i]);
        }
        *w++ = ']';
        *w = '\0';
        free(parts);
        return out;
    oom_arr:
        for (int i = 0; i < n; i++) free(parts[i]);
        free(parts);
        return NULL;
    }
    if (cJSON_IsObject(v)) {
        return canonicalize_object(v);
    }
    return strdup("null");
}

static int cmp_cstr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static char *canonicalize_object(const cJSON *obj) {
    int n = 0;
    cJSON *c;
    cJSON_ArrayForEach(c, obj) n++;
    if (n == 0) return strdup("{}");

    const char **keys = malloc(sizeof(char*) * n);
    if (!keys) return NULL;
    int i = 0;
    cJSON_ArrayForEach(c, obj) keys[i++] = c->string ? c->string : "";
    qsort(keys, n, sizeof(char*), cmp_cstr);

    size_t total = 3; /* {}\0 */
    char **parts = malloc(sizeof(char*) * n);
    for (i = 0; i < n; i++) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, keys[i]);
        char *v = canonicalize_value(item);
        if (!v) { for (int j = 0; j < i; j++) free(parts[j]); free(parts); free(keys); return NULL; }
        /* "key":value */
        size_t klen = strlen(keys[i]);
        size_t vlen = strlen(v);
        size_t plen = klen + vlen + 4;
        char *p = malloc(plen);
        snprintf(p, plen, "\"%s\":%s", keys[i], v);
        free(v);
        parts[i] = p;
        total += plen;
    }

    char *out = malloc(total);
    char *w = out;
    *w++ = '{';
    for (i = 0; i < n; i++) {
        if (i) *w++ = ',';
        size_t l = strlen(parts[i]);
        memcpy(w, parts[i], l);
        w += l;
        free(parts[i]);
    }
    *w++ = '}';
    *w = '\0';
    free(parts);
    free(keys);
    return out;
}

/* ---- HMAC ---- */

static int hmac_sha256(const uint8_t *key, size_t key_len,
                       const uint8_t *data, size_t data_len,
                       uint8_t out[CRAW_HIVE_HMAC_BYTES]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    return mbedtls_md_hmac(info, key, key_len, data, data_len, out);
}

/* Build the canonical signed string: "<type>|<nonce>|<ts>|<payload-canonical>" */
static char *sign_input(const char *type_name, const char *nonce,
                        int64_t ts, const char *payload_canon) {
    size_t tlen = strlen(type_name);
    size_t nlen = strlen(nonce);
    size_t plen = strlen(payload_canon);
    size_t total = tlen + 1 + nlen + 1 + 24 + 1 + plen + 1;
    char *out = malloc(total);
    if (!out) return NULL;
    snprintf(out, total, "%s|%s|%" PRId64 "|%s", type_name, nonce, ts, payload_canon);
    return out;
}

/* ---- Encode ---- */

int craw_hive_proto_encode(const craw_hive_msg_t *msg,
                           const uint8_t *key, size_t key_len,
                           uint8_t **out, size_t *out_len) {
    if (!msg || !key || !out || !out_len) return -1;
    *out = NULL; *out_len = 0;

    /* Parse payload_json (may be NULL → {}). */
    cJSON *payload = NULL;
    if (msg->payload_json && msg->payload_json[0]) {
        payload = cJSON_Parse(msg->payload_json);
        if (!payload || !cJSON_IsObject(payload)) {
            cJSON_Delete(payload);
            return -1;
        }
    } else {
        payload = cJSON_CreateObject();
    }

    char *payload_canon = canonicalize_object(payload);
    if (!payload_canon) { cJSON_Delete(payload); return -1; }

    const char *type_name = craw_hive_msg_type_name(msg->type);
    char *to_sign = sign_input(type_name, msg->nonce_hex, msg->ts, payload_canon);
    if (!to_sign) { free(payload_canon); cJSON_Delete(payload); return -1; }

    uint8_t mac[CRAW_HIVE_HMAC_BYTES];
    if (hmac_sha256(key, key_len, (const uint8_t *)to_sign, strlen(to_sign), mac) != 0) {
        free(to_sign); free(payload_canon); cJSON_Delete(payload); return -1;
    }
    char mac_hex[CRAW_HIVE_HMAC_BYTES * 2 + 1];
    bin2hex(mac, sizeof(mac), mac_hex);

    /* Build envelope JSON (order doesn't matter for transmission, only for signing). */
    cJSON *env = cJSON_CreateObject();
    cJSON_AddStringToObject(env, "type",  type_name);
    cJSON_AddStringToObject(env, "from",  msg->from);
    cJSON_AddStringToObject(env, "to",    msg->to);
    cJSON_AddStringToObject(env, "nonce", msg->nonce_hex);
    cJSON_AddNumberToObject(env, "ts",    (double)msg->ts);
    cJSON_AddItemToObject(env,   "payload", payload);  /* transfers ownership */
    cJSON_AddStringToObject(env, "auth",  mac_hex);

    char *env_json = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    free(to_sign); free(payload_canon);

    if (!env_json) return -1;
    size_t jlen = strlen(env_json);
    if (jlen > CRAW_HIVE_MAX_FRAME - 4) {
        ESP_LOGW(TAG, "frame too large: %zu", jlen);
        free(env_json);
        return -1;
    }
    uint8_t *frame = malloc(4 + jlen);
    if (!frame) { free(env_json); return -1; }
    frame[0] = (jlen >> 24) & 0xFF;
    frame[1] = (jlen >> 16) & 0xFF;
    frame[2] = (jlen >>  8) & 0xFF;
    frame[3] =  jlen        & 0xFF;
    memcpy(frame + 4, env_json, jlen);
    free(env_json);
    *out = frame;
    *out_len = 4 + jlen;
    return 0;
}

/* ---- Decode ---- */

int craw_hive_proto_decode(const uint8_t *frame, size_t frame_len,
                           const uint8_t *key, size_t key_len,
                           int64_t now_epoch,
                           craw_hive_msg_t *out) {
    if (!frame || frame_len < 5 || !out) return CRAW_HIVE_REJECT_AUTH;
    memset(out, 0, sizeof(*out));

    uint32_t jlen = ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16)
                  | ((uint32_t)frame[2] <<  8) |  (uint32_t)frame[3];
    if (jlen + 4 != frame_len || jlen > CRAW_HIVE_MAX_FRAME) {
        return CRAW_HIVE_REJECT_AUTH;
    }

    /* cJSON needs a NUL-terminated input; copy. */
    char *buf = malloc(jlen + 1);
    if (!buf) return CRAW_HIVE_REJECT_AUTH;
    memcpy(buf, frame + 4, jlen);
    buf[jlen] = '\0';

    cJSON *env = cJSON_Parse(buf);
    free(buf);
    if (!env || !cJSON_IsObject(env)) { cJSON_Delete(env); return CRAW_HIVE_REJECT_AUTH; }

    const cJSON *j_type    = cJSON_GetObjectItemCaseSensitive(env, "type");
    const cJSON *j_from    = cJSON_GetObjectItemCaseSensitive(env, "from");
    const cJSON *j_to      = cJSON_GetObjectItemCaseSensitive(env, "to");
    const cJSON *j_nonce   = cJSON_GetObjectItemCaseSensitive(env, "nonce");
    const cJSON *j_ts      = cJSON_GetObjectItemCaseSensitive(env, "ts");
    const cJSON *j_payload = cJSON_GetObjectItemCaseSensitive(env, "payload");
    const cJSON *j_auth    = cJSON_GetObjectItemCaseSensitive(env, "auth");

    if (!cJSON_IsString(j_type) || !cJSON_IsString(j_from) ||
        !cJSON_IsString(j_to) || !cJSON_IsString(j_nonce) ||
        !cJSON_IsNumber(j_ts) || !cJSON_IsObject(j_payload) ||
        !cJSON_IsString(j_auth)) {
        cJSON_Delete(env); return CRAW_HIVE_REJECT_AUTH;
    }

    int64_t ts = (int64_t)j_ts->valuedouble;
    if (now_epoch > 0) {
        int64_t diff = ts - now_epoch;
        if (diff < -CRAW_HIVE_TS_SKEW_SEC || diff > CRAW_HIVE_TS_SKEW_SEC) {
            cJSON_Delete(env); return CRAW_HIVE_REJECT_TS_SKEW;
        }
    }

    /* Verify HMAC on (type|nonce|ts|canonical(payload)). */
    char *payload_canon = canonicalize_object(j_payload);
    if (!payload_canon) { cJSON_Delete(env); return CRAW_HIVE_REJECT_AUTH; }
    char *to_sign = sign_input(j_type->valuestring, j_nonce->valuestring,
                               ts, payload_canon);
    free(payload_canon);
    if (!to_sign) { cJSON_Delete(env); return CRAW_HIVE_REJECT_AUTH; }

    uint8_t expect[CRAW_HIVE_HMAC_BYTES];
    if (hmac_sha256(key, key_len, (const uint8_t *)to_sign, strlen(to_sign), expect) != 0) {
        free(to_sign); cJSON_Delete(env); return CRAW_HIVE_REJECT_AUTH;
    }
    free(to_sign);

    uint8_t got[CRAW_HIVE_HMAC_BYTES];
    if (hex2bin(j_auth->valuestring, got, sizeof(got)) != 0) {
        cJSON_Delete(env); return CRAW_HIVE_REJECT_AUTH;
    }
    /* constant-time compare */
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(expect); i++) diff |= (expect[i] ^ got[i]);
    if (diff != 0) { cJSON_Delete(env); return CRAW_HIVE_REJECT_AUTH; }

    /* Populate out. */
    out->type = craw_hive_msg_type_parse(j_type->valuestring);
    strncpy(out->from,      j_from->valuestring,  CRAW_HIVE_ID_MAX);
    strncpy(out->to,        j_to->valuestring,    CRAW_HIVE_ID_MAX);
    strncpy(out->nonce_hex, j_nonce->valuestring, CRAW_HIVE_NONCE_BYTES * 2);
    out->ts = ts;
    out->payload_json = cJSON_PrintUnformatted(j_payload);

    cJSON_Delete(env);
    if (!out->payload_json) return CRAW_HIVE_REJECT_AUTH;
    return 0;
}
