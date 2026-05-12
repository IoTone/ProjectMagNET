#include "http_vitals.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"        /* tskIDLE_PRIORITY for httpd config */
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "craw_mr60bha2.h"
#include "craw_bh1750.h"

static const char *TAG = "http_vitals";

static httpd_handle_t s_server = NULL;

/* Body-size budgets for the chunked-to-one-shot conversion. Sized for the
 * maximum payload each endpoint can produce, with a small margin:
 *   HR/BR history : 60 samples × ~30 chars + {"samples":[]} wrapper
 *   targets       : 3 max × ~60 chars + {"count":N,"targets":[]} wrapper
 *   phases        : 3 channels × 200 samples × ~9 chars + brackets/commas
 *                   (heap-allocated because ~5 KB is too large for the
 *                   httpd task stack).
 */
#define HIST_JSON_CAP      2048
#define TARGETS_JSON_CAP    384
#define PHASES_JSON_CAP    6144

/* ───── Helpers ─────────────────────────────────────────────────────── */

static void cors_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
    httpd_resp_set_hdr(req, "Cache-Control",                "no-cache, no-store, must-revalidate");
}

static esp_err_t send_json(httpd_req_t *req, const char *body, size_t len) {
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, len);
}

/* Append `frag` to `buf` at offset `*off`, advancing `*off` and bounds-checking
 * against `cap`. Returns ESP_FAIL if the fragment would overflow (rare given
 * the conservative caps above, but kept defensive so corruption is impossible).
 */
static inline esp_err_t json_append(char *buf, size_t cap, size_t *off,
                                    const char *frag, size_t frag_len) {
    if (*off + frag_len >= cap) return ESP_FAIL;
    memcpy(buf + *off, frag, frag_len);
    *off += frag_len;
    return ESP_OK;
}

/* ───── /vitals ─────────────────────────────────────────────────────── */

static esp_err_t h_vitals(httpd_req_t *req) {
    craw_mr60_state_t s;
    craw_mr60_get_state(&s);
    float lux = 0.0f;
    bool  has_lux = (craw_bh1750_read(&lux) == ESP_OK);

    /* Build the lux fragment as either a number or `null`, then splice it in. */
    char lux_frag[32];
    if (has_lux) snprintf(lux_frag, sizeof(lux_frag), "%.1f", lux);
    else         snprintf(lux_frag, sizeof(lux_frag), "null");

    char body[512];
    int n = snprintf(body, sizeof(body),
        "{\"bpm\":%.1f,\"rpm\":%.1f,\"presence\":%s,"
        "\"distance_cm\":%d,\"range_flag\":%lu,"
        "\"lux\":%s,"
        "\"total_phase\":%.4f,\"breath_phase\":%.4f,\"heart_phase\":%.4f,"
        "\"target_count\":%u,"
        "\"fw_version\":\"0x%08lx\","
        "\"timestamp_us\":%lld}",
        s.bpm, s.rpm,
        s.present ? "true" : "false",
        (s.range_flag != 0) ? (int)(s.distance_m * 100.0f + 0.5f) : 0,
        (unsigned long)s.range_flag,
        lux_frag,
        s.total_phase, s.breath_phase, s.heart_phase,
        (unsigned)s.target_count,
        (unsigned long)s.fw_version,
        (long long)esp_timer_get_time());
    return send_json(req, body, (n > 0 && n < (int)sizeof(body)) ? (size_t)n : strlen(body));
}

/* ───── /heart-rate, /breathing, /presence, /lux ────────────────────── */

static esp_err_t h_hr(httpd_req_t *req) {
    craw_mr60_state_t s;
    craw_mr60_get_state(&s);
    char body[160];
    int n = snprintf(body, sizeof(body),
        "{\"bpm\":%.1f,\"presence\":%s,\"timestamp_us\":%lld}",
        s.bpm, s.present ? "true" : "false",
        (long long)s.bpm_updated_us);
    return send_json(req, body, (size_t)n);
}

static esp_err_t h_br(httpd_req_t *req) {
    craw_mr60_state_t s;
    craw_mr60_get_state(&s);
    char body[160];
    int n = snprintf(body, sizeof(body),
        "{\"rpm\":%.1f,\"timestamp_us\":%lld}",
        s.rpm, (long long)s.rpm_updated_us);
    return send_json(req, body, (size_t)n);
}

static esp_err_t h_presence(httpd_req_t *req) {
    craw_mr60_state_t s;
    craw_mr60_get_state(&s);
    int64_t age_ms = (s.presence_updated_us > 0)
        ? (esp_timer_get_time() - s.presence_updated_us) / 1000 : -1;
    char body[160];
    int n = snprintf(body, sizeof(body),
        "{\"present\":%s,\"distance_cm\":%d,\"age_ms\":%lld,\"timestamp_us\":%lld}",
        s.present ? "true" : "false",
        (s.range_flag != 0) ? (int)(s.distance_m * 100.0f + 0.5f) : 0,
        (long long)age_ms, (long long)s.presence_updated_us);
    return send_json(req, body, (size_t)n);
}

static esp_err_t h_lux(httpd_req_t *req) {
    float lux = 0.0f;
    bool ok = (craw_bh1750_read(&lux) == ESP_OK);
    char body[128];
    int n;
    if (ok) {
        n = snprintf(body, sizeof(body),
            "{\"lux\":%.1f,\"timestamp_us\":%lld}",
            lux, (long long)esp_timer_get_time());
    } else {
        n = snprintf(body, sizeof(body),
            "{\"lux\":null,\"error\":\"i2c_read_failed\"}");
    }
    return send_json(req, body, (size_t)n);
}

/* ───── /heart-rate/history, /breathing/history (one-shot) ───────────── */

/* One snprintf chain into a stack buffer, then a single httpd_resp_send.
 *
 * Why this is the right shape: chunked encoding made every sample a separate
 * send() syscall, which saturated LWIP's send queue under back-to-back
 * requests and triggered the `httpd_sock_err send:11` (EAGAIN) we see in
 * the proxy logs. A single send() per response keeps the TCP path quiet.
 *
 * The buffers used to live in `static` storage and were corrupted if two
 * clients hit this handler concurrently. Stack-local arrays here size at
 * ~480 + ~240 = ~720 B per call — well within the 12 KB httpd task stack.
 */
static esp_err_t send_history(httpd_req_t *req, const char *kind) {
    uint64_t t_ms[CRAW_MR60_HISTORY_LEN];
    float    v[CRAW_MR60_HISTORY_LEN];
    size_t n = (kind[0] == 'h')
        ? craw_mr60_get_hr_history(t_ms, v, CRAW_MR60_HISTORY_LEN)
        : craw_mr60_get_rr_history(t_ms, v, CRAW_MR60_HISTORY_LEN);

    char body[HIST_JSON_CAP];
    size_t off = 0;
    json_append(body, sizeof(body), &off, "{\"samples\":[", 12);
    for (size_t i = 0; i < n; i++) {
        int len = snprintf(body + off, sizeof(body) - off,
            "%s{\"t\":%llu,\"v\":%.1f}",
            (i == 0) ? "" : ",",
            (unsigned long long)t_ms[i], v[i]);
        if (len < 0 || (size_t)len >= sizeof(body) - off) break;
        off += (size_t)len;
    }
    json_append(body, sizeof(body), &off, "]}", 2);

    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, off);
}

static esp_err_t h_hr_hist(httpd_req_t *req) { return send_history(req, "hr"); }
static esp_err_t h_br_hist(httpd_req_t *req) { return send_history(req, "br"); }

/* ───── /targets (one-shot) ─────────────────────────────────────────── */

static esp_err_t h_targets(httpd_req_t *req) {
    craw_mr60_target_t targets[CRAW_MR60_MAX_TARGETS];
    size_t n = craw_mr60_get_targets(targets);

    char body[TARGETS_JSON_CAP];
    int hl = snprintf(body, sizeof(body), "{\"count\":%u,\"targets\":[", (unsigned)n);
    size_t off = (hl > 0) ? (size_t)hl : 0;
    for (size_t i = 0; i < n; i++) {
        int len = snprintf(body + off, sizeof(body) - off,
            "%s{\"id\":%u,\"x_m\":%.3f,\"y_m\":%.3f,\"dop\":%ld,\"cluster\":%ld}",
            (i == 0) ? "" : ",",
            (unsigned)i, targets[i].x_m, targets[i].y_m,
            (long)targets[i].dop_index, (long)targets[i].cluster_index);
        if (len < 0 || (size_t)len >= sizeof(body) - off) break;
        off += (size_t)len;
    }
    json_append(body, sizeof(body), &off, "]}", 2);

    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, off);
}

/* ───── /phases (one-shot, heap-allocated) ──────────────────────────── */

/* Append a single float channel `[v0,v1,...]` to `body`. */
static esp_err_t append_channel(char *body, size_t cap, size_t *off,
                                const float *vals, size_t n) {
    if (json_append(body, cap, off, "[", 1) != ESP_OK) return ESP_FAIL;
    for (size_t i = 0; i < n; i++) {
        int len = snprintf(body + *off, cap - *off,
            "%s%.4f", (i == 0) ? "" : ",", vals[i]);
        if (len < 0 || (size_t)len >= cap - *off) return ESP_FAIL;
        *off += (size_t)len;
    }
    return json_append(body, cap, off, "]", 1);
}

static esp_err_t h_phases(httpd_req_t *req) {
    /* Heap-allocate the phase channels + the JSON body — three 200-sample
     * float arrays (2.4 KB) plus the ~5 KB output buffer is too much to put
     * on the httpd task stack alongside snprintf scratch. The prior `static`
     * arrays were also a concurrency bug — two clients hitting /phases
     * concurrently would corrupt each other's data. */
    float *heart  = malloc(CRAW_MR60_PHASE_HISTORY_LEN * sizeof(float));
    float *breath = malloc(CRAW_MR60_PHASE_HISTORY_LEN * sizeof(float));
    float *total  = malloc(CRAW_MR60_PHASE_HISTORY_LEN * sizeof(float));
    char  *body   = malloc(PHASES_JSON_CAP);
    if (!heart || !breath || !total || !body) {
        free(heart); free(breath); free(total); free(body);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t n = craw_mr60_get_phase_history(NULL, heart, breath, total,
                                           CRAW_MR60_PHASE_HISTORY_LEN);

    size_t off = 0;
    esp_err_t e = ESP_OK;
    if (e == ESP_OK) e = json_append(body, PHASES_JSON_CAP, &off, "[", 1);
    if (e == ESP_OK) e = append_channel(body, PHASES_JSON_CAP, &off, heart,  n);
    if (e == ESP_OK) e = json_append(body, PHASES_JSON_CAP, &off, ",", 1);
    if (e == ESP_OK) e = append_channel(body, PHASES_JSON_CAP, &off, breath, n);
    if (e == ESP_OK) e = json_append(body, PHASES_JSON_CAP, &off, ",", 1);
    if (e == ESP_OK) e = append_channel(body, PHASES_JSON_CAP, &off, total,  n);
    if (e == ESP_OK) e = json_append(body, PHASES_JSON_CAP, &off, "]", 1);

    if (e != ESP_OK) {
        free(heart); free(breath); free(total); free(body);
        ESP_LOGW(TAG, "h_phases: body buffer too small, sending 500");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t rc = httpd_resp_send(req, body, off);
    free(heart); free(breath); free(total); free(body);
    return rc;
}

/* ───── OPTIONS preflight (wildcard) ────────────────────────────────── */

static esp_err_t h_options(httpd_req_t *req) {
    cors_headers(req);
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ───── Route table + start/stop ────────────────────────────────────── */

#define R_GET(path, fn)  { .uri = (path), .method = HTTP_GET,    .handler = (fn), .user_ctx = NULL }

static const httpd_uri_t s_routes_get[] = {
    R_GET("/vitals",              h_vitals),
    R_GET("/heart-rate",          h_hr),
    R_GET("/heart-rate/history",  h_hr_hist),
    R_GET("/breathing",           h_br),
    R_GET("/breathing/history",   h_br_hist),
    R_GET("/presence",            h_presence),
    R_GET("/lux",                 h_lux),
    R_GET("/targets",             h_targets),
    R_GET("/phases",              h_phases),
};
static const httpd_uri_t s_route_options = {
    .uri = "/*", .method = HTTP_OPTIONS, .handler = h_options, .user_ctx = NULL,
};

esp_err_t http_vitals_start(void) {
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn  = httpd_uri_match_wildcard;   // enables "/*" OPTIONS
    cfg.max_uri_handlers = sizeof(s_routes_get) / sizeof(s_routes_get[0]) + 4;
    cfg.lru_purge_enable = true;

    /* Stability tuning — see commit message for the full reasoning. Brief:
     *   - max_open_sockets 7 -> 13: the C6 keeps recently-closed sockets in
     *     TIME_WAIT for a few seconds, and with 4 d3-spatial cells polling
     *     simultaneously we'd hit the 7-socket ceiling and start dropping.
     *   - recv/send_wait_timeout: bound how long the server task blocks on
     *     a wedged peer. Without these, a slow proxy can hold a worker task
     *     long enough that the task WDT trips and the device reboots.
     *   - task_priority: nudge one above the IDF default so handler work
     *     doesn't get preempted endlessly by lower-priority background tasks.
     *     Still well below WiFi (priority 23) so we don't starve the radio.
     *   - stack_size 8 -> 12 KB: gives snprintf chains headroom; one-shot
     *     handlers now build the whole JSON locally and the prior 8 KB was
     *     tight under -Og.
     */
    cfg.max_open_sockets  = 13;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    cfg.task_priority     = tskIDLE_PRIORITY + 6;
    cfg.stack_size        = 12288;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    for (size_t i = 0; i < sizeof(s_routes_get) / sizeof(s_routes_get[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &s_routes_get[i]));
    }
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &s_route_options));

    ESP_LOGI(TAG, "vitals HTTP started — %d GET routes + wildcard OPTIONS",
             (int)(sizeof(s_routes_get) / sizeof(s_routes_get[0])));
    return ESP_OK;
}

void http_vitals_stop(void) {
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "vitals HTTP stopped");
}
