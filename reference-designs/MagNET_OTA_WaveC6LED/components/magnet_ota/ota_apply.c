/*
 * ota_apply — evaluate a VERIFIED bundle into the running dictionary, roll back
 * if it misbehaves, and tell the server what happened.
 *
 * This is the phase the whole system exists for. Everything before it moves
 * bytes; this is where a device changes what it DOES without being reflashed.
 *
 * Two invariants:
 *
 *   1. NOTHING UNVERIFIED IS EVER EVALUATED. ota_fetch_and_verify() must have
 *      returned true for this release. Evaluating first and checking afterwards
 *      would be no verification at all, since evaluation is the dangerous part.
 *
 *   2. A FAILED APPLY LEAVES NO TRACE. Savepoint first, restore on any error.
 *      The dictionary is append-only and lookup searches backward, so a
 *      redefinition shadows rather than mutates and truncation genuinely undoes
 *      the bundle — including restoring any word it overrode.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "magnet_ota.h"
#include "magnet_transport.h"
#include "magnet_cfg.h"
#include "forth_core.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "ota_apply";
static char s_apply_status[96] = "never applied";

const char *ota_apply_status(void) { return s_apply_status; }

/* ---- Bundle persistence (punch-list H4) ----------------------------------
 * The dictionary is RAM; without this, a power cycle silently reverts the
 * device's behaviour while NVS CFG_APPLIED_ID still reports it up to date —
 * the worst kind of wrong: looks upgraded, isn't. Persist the verified
 * bundle text after a successful apply and re-evaluate it at boot. */
#define OTA_NVS_NS  "magnet_ota"
#define OTA_NVS_KEY "bundle"

static void persist_bundle(const uint8_t *text, size_t len) {
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "bundle persist: nvs_open failed (RAM-only until next apply)");
        return;
    }
    esp_err_t e1 = nvs_set_blob(h, OTA_NVS_KEY, text, len);
    esp_err_t e2 = nvs_commit(h);
    nvs_close(h);
    if (e1 != ESP_OK || e2 != ESP_OK)
        ESP_LOGW(TAG, "bundle persist failed (%d/%d) — RAM-only until next apply", e1, e2);
}

int ota_apply_saved(void) {
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = 0;
    if (nvs_get_blob(h, OTA_NVS_KEY, NULL, &len) != ESP_OK || len == 0) {
        nvs_close(h);
        return 0;
    }
    uint8_t *text = malloc(len);
    if (!text) { nvs_close(h); return -1; }
    esp_err_t err = nvs_get_blob(h, OTA_NVS_KEY, text, &len);
    nvs_close(h);
    if (err != ESP_OK) { free(text); return -1; }

    char fail[80];
    int rc = forth_eval_rollback((const char *)text, len, fail, sizeof fail);
    free(text);
    if (rc == 0) {
        ESP_LOGI(TAG, "persisted bundle re-applied at boot (%u bytes)", (unsigned)len);
        return 1;
    }
    /* A bundle that applied cleanly once but fails now means the environment
     * changed under it (e.g. firmware update removed an FFI word). Rolled
     * back; the next check-in will converge on a fresh release. */
    ESP_LOGW(TAG, "persisted bundle failed at boot (rc=%d) at: %s — rolled back", rc, fail);
    return -1;
}

/* Report the outcome. Called for BOTH success and failure — a device that only
 * reports success leaves the operator unable to distinguish "still working on
 * it" from "tried and broke", which is the worse of the two to be blind to. */
static void report(magnet_transport_t *tx, long release_id, bool ok, const char *detail) {
    char body[512], resp[256];
    size_t rlen = 0;
    if (ok) {
        snprintf(body, sizeof body,
                 "{\"release_id\":%ld,\"ok\":true}", release_id);
    } else {
        /* Escape nothing fancy — keep the tail short and strip quotes so a
         * Forth diagnostic containing them cannot break the JSON. The server
         * truncates too, but malformed JSON would be dropped entirely and the
         * failure report is the one message we most want to survive. */
        char safe[192];
        size_t j = 0;
        for (size_t i = 0; detail && detail[i] && j < sizeof safe - 1; ++i) {
            char c = detail[i];
            if (c == '"' || c == '\\' || c < 0x20) c = ' ';
            safe[j++] = c;
        }
        safe[j] = '\0';
        snprintf(body, sizeof body,
                 "{\"release_id\":%ld,\"ok\":false,\"error\":\"%s\"}", release_id, safe);
    }
    int st = tx->request(tx, "POST", "/api/devices/apply-result",
                         body, resp, sizeof resp, &rlen);
    if (st != 200)
        ESP_LOGW(TAG, "apply-result POST returned %d (outcome recorded locally only)", st);
    else
        ESP_LOGI(TAG, "apply-result reported: release %ld %s", release_id, ok ? "OK" : "FAILED");
}

bool ota_apply(magnet_transport_t *tx, const ota_release_t *rel) {
    size_t len = 0;
    const uint8_t *bundle = ota_bundle(&len);
    if (!bundle || len == 0) {
        snprintf(s_apply_status, sizeof s_apply_status, "no verified bundle");
        return false;
    }

    ESP_LOGI(TAG, "applying release %ld (%u bytes) at dict used=%d",
             rel->release_id, (unsigned)len, forth_heap_used());

    /* The engine's verified-apply primitive (ESPIDFORTH >= 0.4.0) owns the
     * savepoint / line-at-a-time / rollback dance — shared with the hive's
     * craw_role_bundle path (punch-list H4). It evaluates a private copy, so
     * `bundle` stays pristine for persistence below. */
    char fail[80];
    int rc = forth_eval_rollback((const char *)bundle, len, fail, sizeof fail);
    if (rc != 0) {
        if (rc < 0)
            snprintf(s_apply_status, sizeof s_apply_status, "no memory");
        else
            snprintf(s_apply_status, sizeof s_apply_status, "failed at: %.60s", fail);
        ESP_LOGE(TAG, "apply FAILED, rolled back — %s", s_apply_status);
        report(tx, rel->release_id, false, s_apply_status);
        return false;
    }

    /* Survive reboot: the dictionary is RAM, so persist the verified text and
     * re-apply it at boot (ota_apply_saved). Persist BEFORE the cfg markers so
     * a power cut between the two re-applies rather than silently reverts. */
    persist_bundle(bundle, len);

    /* Only now is the device genuinely running this release. Record it BEFORE
     * reporting: if the POST fails we are still correct locally, and the next
     * check-in resolves to noop instead of re-applying forever. */
    char idbuf[24];
    snprintf(idbuf, sizeof idbuf, "%ld", rel->release_id);
    cfg_set(CFG_APPLIED_ID, idbuf);
    cfg_set(CFG_APPLIED_VER, rel->version);

    snprintf(s_apply_status, sizeof s_apply_status, "applied %s", rel->version);
    ESP_LOGI(TAG, "apply OK: now running release %ld version %s (dict used=%d)",
             rel->release_id, rel->version, forth_heap_used());
    report(tx, rel->release_id, true, NULL);
    return true;
}
