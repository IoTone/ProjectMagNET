#include "http_vitals.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "craw_mr60bha2.h"
#include "craw_bh1750.h"

static const char *TAG = "http_vitals";

static httpd_handle_t s_server = NULL;

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

static esp_err_t send_json_chunk_open(httpd_req_t *req) {
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
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

/* ───── /heart-rate/history, /breathing/history (chunked) ───────────── */

static esp_err_t send_history(httpd_req_t *req, const char *kind) {
    static uint64_t t_ms[CRAW_MR60_HISTORY_LEN];
    static float    v[CRAW_MR60_HISTORY_LEN];
    size_t n = (kind[0] == 'h')
        ? craw_mr60_get_hr_history(t_ms, v, CRAW_MR60_HISTORY_LEN)
        : craw_mr60_get_rr_history(t_ms, v, CRAW_MR60_HISTORY_LEN);

    send_json_chunk_open(req);
    httpd_resp_send_chunk(req, "{\"samples\":[", 12);
    char item[64];
    for (size_t i = 0; i < n; i++) {
        int len = snprintf(item, sizeof(item),
            "%s{\"t\":%llu,\"v\":%.1f}",
            (i == 0) ? "" : ",",
            (unsigned long long)t_ms[i], v[i]);
        httpd_resp_send_chunk(req, item, (size_t)len);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);  /* terminate */
    return ESP_OK;
}

static esp_err_t h_hr_hist(httpd_req_t *req) { return send_history(req, "hr"); }
static esp_err_t h_br_hist(httpd_req_t *req) { return send_history(req, "br"); }

/* ───── /targets ────────────────────────────────────────────────────── */

static esp_err_t h_targets(httpd_req_t *req) {
    craw_mr60_target_t targets[CRAW_MR60_MAX_TARGETS];
    size_t n = craw_mr60_get_targets(targets);

    send_json_chunk_open(req);
    char hdr[48];
    int hl = snprintf(hdr, sizeof(hdr), "{\"count\":%u,\"targets\":[", (unsigned)n);
    httpd_resp_send_chunk(req, hdr, (size_t)hl);

    char item[160];
    for (size_t i = 0; i < n; i++) {
        int len = snprintf(item, sizeof(item),
            "%s{\"id\":%u,\"x_m\":%.3f,\"y_m\":%.3f,\"dop\":%ld,\"cluster\":%ld}",
            (i == 0) ? "" : ",",
            (unsigned)i, targets[i].x_m, targets[i].y_m,
            (long)targets[i].dop_index, (long)targets[i].cluster_index);
        httpd_resp_send_chunk(req, item, (size_t)len);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ───── /phases — streamgraph distributions[] shape ─────────────────── */

static esp_err_t send_channel(httpd_req_t *req, const float *vals, size_t n) {
    char buf[24];
    httpd_resp_send_chunk(req, "[", 1);
    for (size_t i = 0; i < n; i++) {
        int len = snprintf(buf, sizeof(buf),
            "%s%.4f", (i == 0) ? "" : ",", vals[i]);
        httpd_resp_send_chunk(req, buf, (size_t)len);
    }
    httpd_resp_send_chunk(req, "]", 1);
    return ESP_OK;
}

static esp_err_t h_phases(httpd_req_t *req) {
    static float heart [CRAW_MR60_PHASE_HISTORY_LEN];
    static float breath[CRAW_MR60_PHASE_HISTORY_LEN];
    static float total [CRAW_MR60_PHASE_HISTORY_LEN];
    size_t n = craw_mr60_get_phase_history(NULL, heart, breath, total,
                                           CRAW_MR60_PHASE_HISTORY_LEN);
    send_json_chunk_open(req);
    httpd_resp_send_chunk(req, "[", 1);
    send_channel(req, heart,  n);
    httpd_resp_send_chunk(req, ",", 1);
    send_channel(req, breath, n);
    httpd_resp_send_chunk(req, ",", 1);
    send_channel(req, total,  n);
    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
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
    cfg.stack_size = 8192;

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
