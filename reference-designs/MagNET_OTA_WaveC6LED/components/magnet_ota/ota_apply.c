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
#include "magnet_ota.h"
#include "magnet_transport.h"
#include "magnet_cfg.h"
#include "forth_core.h"
#include "esp_log.h"

static const char *TAG = "ota_apply";
static char s_apply_status[96] = "never applied";

const char *ota_apply_status(void) { return s_apply_status; }

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

    /* forth_eval() takes a C string; the bundle is bytes off the wire. */
    char *text = malloc(len + 1);
    if (!text) { snprintf(s_apply_status, sizeof s_apply_status, "no memory"); return false; }
    memcpy(text, bundle, len);
    text[len] = '\0';

    forth_savepoint_t sp;
    forth_save(&sp);
    int errs_before = forth_error_count();

    ESP_LOGI(TAG, "applying release %ld (%u bytes) at dict=%d code=%d",
             rel->release_id, (unsigned)len, sp.dict_count, sp.code_ptr);

    /* Line at a time: the interpreter is line-oriented, and stopping at the
     * FIRST bad line means the rollback message names the actual problem
     * instead of the last line of the file. */
    bool ok = true;
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        forth_eval(line);
        if (forth_error_count() != errs_before) {
            snprintf(s_apply_status, sizeof s_apply_status, "failed at: %.60s", line);
            ok = false;
            break;
        }
    }
    free(text);

    if (!ok) {
        forth_restore(&sp);
        ESP_LOGE(TAG, "apply FAILED, rolled back to dict=%d code=%d — %s",
                 sp.dict_count, sp.code_ptr, s_apply_status);
        report(tx, rel->release_id, false, s_apply_status);
        return false;
    }

    /* Only now is the device genuinely running this release. Record it BEFORE
     * reporting: if the POST fails we are still correct locally, and the next
     * check-in resolves to noop instead of re-applying forever. */
    char idbuf[24];
    snprintf(idbuf, sizeof idbuf, "%ld", rel->release_id);
    cfg_set(CFG_APPLIED_ID, idbuf);
    cfg_set(CFG_APPLIED_VER, rel->version);

    snprintf(s_apply_status, sizeof s_apply_status, "applied %s", rel->version);
    ESP_LOGI(TAG, "apply OK: now running release %ld version %s (dict %d -> %d)",
             rel->release_id, rel->version, sp.dict_count, forth_heap_used() ? 0 : 0);
    report(tx, rel->release_id, true, NULL);
    return true;
}
