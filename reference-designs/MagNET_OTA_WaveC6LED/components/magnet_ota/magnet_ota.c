/*
 * magnet_ota — check-in against RobotARme's POST /api/devices/check-in.
 *
 * Server contract (docs/API.md in the RobotARme repo):
 *   request  { current_version?, current_release_id?, telemetry? }
 *   response { action: "noop"|"update", server_time, ... }
 *   on update, also: release{}, download_url, signature_url, expires_unix
 *
 * The server enforces a 5 s per-device cooldown and answers 429 if you poll
 * faster. That is not an error condition to panic about — it means this device
 * is asking too often — so it is reported distinctly.
 */
#include <string.h>
#include <stdio.h>
#include "magnet_ota.h"
#include "magnet_transport.h"
#include "magnet_cfg.h"
#include "forth_core.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "magnet_ota";

/* 2 KB: a check-in response carries a release descriptor plus two signed URLs,
 * which measures a few hundred bytes. The transport flags truncation rather
 * than letting a clipped body parse as "noop". */
#define RESP_CAP 2048

/*
 * Check-ins are SERIALISED and the response buffer is STATIC.
 *
 * Two callers exist — the 60 s poll task and the console's `checkin` command —
 * and nothing stopped them overlapping. Worse, a 2 KB response buffer as a
 * local put ~2.4 KB of locals on a 6 KB task stack before esp_http_client added
 * its own frames, which is how you get corruption that presents as sockets
 * failing rather than as an obvious overflow.
 */
static SemaphoreHandle_t s_lock;
static char s_resp[RESP_CAP];

static magnet_transport_t *s_tx;
static ota_release_t s_pending;
static bool s_have_pending;
static char s_status[64] = "idle";

esp_err_t ota_init(void) {
    s_tx = transport_ip();
    if (!s_tx) return ESP_FAIL;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    return s_tx->open(s_tx);
}

const ota_release_t *ota_pending(void) { return s_have_pending ? &s_pending : NULL; }
const char *ota_last_status(void) { return s_status; }

static void jstr(cJSON *o, const char *key, char *dst, size_t cap) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    dst[0] = '\0';
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, cap - 1);
        dst[cap - 1] = '\0';
    }
}

ota_action_t ota_checkin(void) {
    size_t rlen = 0;
    char body[192];
    char ver[CFG_MAX];

    if (!s_tx) { snprintf(s_status, sizeof s_status, "not initialised"); return OTA_ERROR; }
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(15000)) != pdTRUE) {
        snprintf(s_status, sizeof s_status, "busy");
        return OTA_ERROR;
    }
    if (!cfg_get(CFG_SERVER_URL, ver, sizeof ver)) {
        snprintf(s_status, sizeof s_status, "no server_url");
        if (s_lock) xSemaphoreGive(s_lock);
        return OTA_ERROR;
    }

    /* Report what we are running so the server can resolve to noop. Nothing
     * has been applied yet, so this is the firmware's own version until D4
     * starts recording applied releases. */
    snprintf(body, sizeof body, "{\"current_version\":\"%s\"}", "0.0.0");

    int st = s_tx->request(s_tx, "POST", "/api/devices/check-in",
                           body, s_resp, sizeof s_resp, &rlen);
    if (s_lock) xSemaphoreGive(s_lock);

    if (st < 0)    { snprintf(s_status, sizeof s_status, "unreachable"); return OTA_ERROR; }
    if (st == 401) { snprintf(s_status, sizeof s_status, "bad token");   return OTA_ERROR; }
    if (st == 429) { snprintf(s_status, sizeof s_status, "cooldown");    return OTA_ERROR; }
    if (st != 200) { snprintf(s_status, sizeof s_status, "http %d", st); return OTA_ERROR; }

    cJSON *root = cJSON_ParseWithLength(s_resp, rlen);
    if (!root) { snprintf(s_status, sizeof s_status, "bad json"); return OTA_ERROR; }

    char action[16];
    jstr(root, "action", action, sizeof action);

    ota_action_t out = OTA_ERROR;
    if (strcmp(action, "noop") == 0) {
        s_have_pending = false;
        snprintf(s_status, sizeof s_status, "up to date");
        out = OTA_NOOP;
    } else if (strcmp(action, "update") == 0) {
        memset(&s_pending, 0, sizeof s_pending);
        jstr(root, "download_url",  s_pending.download_url,  sizeof s_pending.download_url);
        jstr(root, "signature_url", s_pending.signature_url, sizeof s_pending.signature_url);
        cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "expires_unix");
        if (cJSON_IsNumber(exp)) s_pending.expires_unix = (long)exp->valuedouble;

        cJSON *rel = cJSON_GetObjectItemCaseSensitive(root, "release");
        if (rel) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(rel, "id");
            if (cJSON_IsNumber(id)) s_pending.release_id = (long)id->valuedouble;
            jstr(rel, "version",     s_pending.version, sizeof s_pending.version);
            jstr(rel, "name",        s_pending.name,    sizeof s_pending.name);
            jstr(rel, "file_sha256", s_pending.sha256,  sizeof s_pending.sha256);
        }
        s_have_pending = true;
        snprintf(s_status, sizeof s_status, "update %s", s_pending.version);
        ESP_LOGI(TAG, "update available: release %ld version %s",
                 s_pending.release_id, s_pending.version);
        out = OTA_UPDATE;
    } else {
        snprintf(s_status, sizeof s_status, "unknown action");
    }
    cJSON_Delete(root);
    return out;
}

/* --- Forth vocabulary ---------------------------------------------------- */
/* The only part of this component that knows about the Forth engine, so an
 * engine swap touches this and nothing else. */

static void w_ota_checkin(void) { forth_push((intptr_t)ota_checkin()); }

static void w_ota_status(void) {
    /* Push addr/len so Forth can TYPE it — the stub has no string literals but
     * it does have TYPE, which is enough to read a C-owned string. */
    const char *s = ota_last_status();
    forth_push((intptr_t)s);
    forth_push((intptr_t)strlen(s));
}

static void w_ota_release(void) {
    forth_push((intptr_t)(s_have_pending ? s_pending.release_id : 0));
}

void ota_register_forth_words(void) {
    forth_register_word("ota-checkin", w_ota_checkin);
    forth_register_word("ota-status",  w_ota_status);
    forth_register_word("ota-release", w_ota_release);
}
