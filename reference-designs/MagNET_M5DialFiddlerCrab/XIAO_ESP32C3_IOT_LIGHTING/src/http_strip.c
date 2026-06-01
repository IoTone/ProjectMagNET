/*
 * http_strip.c - UC2 neopixel actuator HTTP API
 *
 * Contract (matches the in-XR UC2 actuator panel + mock-join-server):
 *   POST /api/v1/actuator/neopixel
 *     body (all optional, partial update):
 *       { "on": bool,
 *         "brightness_pct": 0..100,
 *         "color": { "r":0..255, "g":0..255, "b":0..255 },
 *         "pattern": "solid|breathing|rainbow|chase|twinkle",
 *         "pattern_speed_pct": 0..100 }
 *   GET  /api/v1/actuator/neopixel  -> current state
 *   OPTIONS (wildcard)              -> CORS preflight (204)
 *
 * Response (both GET and POST):
 *   { on, brightness_pct, color:{r,g,b}, pattern, pattern_speed_pct,
 *     led_count, last_changed_at, available_patterns:[...], timestamp_us }
 */

#include <string.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"
#include "http_strip.h"
#include "strip_ctl.h"

static const char *TAG = "http_strip";
static httpd_handle_t s_server = NULL;

#define NEOPIXEL_URI "/api/v1/actuator/neopixel"

static void cors_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
}

/* Serialize the current strip state to the UC2 response shape. */
static esp_err_t send_state(httpd_req_t *req) {
    strip_state_t s;
    strip_get_state(&s);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "on", s.on);
    cJSON_AddNumberToObject(root, "brightness_pct", s.brightness_pct);
    cJSON *col = cJSON_AddObjectToObject(root, "color");
    cJSON_AddNumberToObject(col, "r", s.r);
    cJSON_AddNumberToObject(col, "g", s.g);
    cJSON_AddNumberToObject(col, "b", s.b);
    cJSON_AddStringToObject(root, "pattern", STRIP_PATTERN_NAMES[s.pattern]);
    cJSON_AddNumberToObject(root, "pattern_speed_pct", s.pattern_speed_pct);
    cJSON_AddNumberToObject(root, "led_count", s.led_count);
    cJSON_AddNumberToObject(root, "last_changed_at",
                            (double)(s.last_changed_us / 1000));
    cJSON *pats = cJSON_AddArrayToObject(root, "available_patterns");
    for (int i = 0; i < PAT_COUNT; i++)
        cJSON_AddItemToArray(pats, cJSON_CreateString(STRIP_PATTERN_NAMES[i]));
    cJSON_AddNumberToObject(root, "timestamp_us",
                            (double)esp_timer_get_time());

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
        return ESP_FAIL;
    }
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return e;
}

static esp_err_t h_get(httpd_req_t *req) {
    return send_state(req);
}

static esp_err_t h_post(httpd_req_t *req) {
    char buf[512];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_set_status(req, "400 Bad Request");
        cors_headers(req);
        httpd_resp_sendstr(req, "{\"error\":\"bad body length\"}");
        return ESP_OK;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            cors_headers(req);
            httpd_resp_sendstr(req, "{\"error\":\"recv failed\"}");
            return ESP_OK;
        }
        got += r;
    }
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        cors_headers(req);
        httpd_resp_sendstr(req, "{\"error\":\"invalid json\"}");
        return ESP_OK;
    }

    bool on_v;            const bool *on_p = NULL;
    int  bri_v;           const int  *bri_p = NULL;
    uint8_t rgb[3];       const uint8_t *rgb_p = NULL;
    strip_pattern_t pat_v;const strip_pattern_t *pat_p = NULL;
    int  spd_v;           const int  *spd_p = NULL;

    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(root, "on");
    if (cJSON_IsBool(j)) { on_v = cJSON_IsTrue(j); on_p = &on_v; }

    j = cJSON_GetObjectItemCaseSensitive(root, "brightness_pct");
    if (cJSON_IsNumber(j)) { bri_v = (int)j->valuedouble; bri_p = &bri_v; }

    cJSON *col = cJSON_GetObjectItemCaseSensitive(root, "color");
    if (cJSON_IsObject(col)) {
        cJSON *jr = cJSON_GetObjectItemCaseSensitive(col, "r");
        cJSON *jg = cJSON_GetObjectItemCaseSensitive(col, "g");
        cJSON *jb = cJSON_GetObjectItemCaseSensitive(col, "b");
        if (cJSON_IsNumber(jr) && cJSON_IsNumber(jg) && cJSON_IsNumber(jb)) {
            int rr = (int)jr->valuedouble, gg = (int)jg->valuedouble,
                bb = (int)jb->valuedouble;
            rgb[0] = (uint8_t)(rr < 0 ? 0 : rr > 255 ? 255 : rr);
            rgb[1] = (uint8_t)(gg < 0 ? 0 : gg > 255 ? 255 : gg);
            rgb[2] = (uint8_t)(bb < 0 ? 0 : bb > 255 ? 255 : bb);
            rgb_p = rgb;
        }
    }

    j = cJSON_GetObjectItemCaseSensitive(root, "pattern");
    if (cJSON_IsString(j) && j->valuestring) {
        int p = strip_pattern_from_name(j->valuestring);
        if (p >= 0) { pat_v = (strip_pattern_t)p; pat_p = &pat_v; }
    }

    j = cJSON_GetObjectItemCaseSensitive(root, "pattern_speed_pct");
    if (cJSON_IsNumber(j)) { spd_v = (int)j->valuedouble; spd_p = &spd_v; }

    cJSON_Delete(root);

    strip_set(on_p, bri_p, rgb_p, pat_p, spd_p);
    return send_state(req);   /* echo updated state, like the mock server */
}

static esp_err_t h_options(httpd_req_t *req) {
    cors_headers(req);
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t s_get = {
    .uri = NEOPIXEL_URI, .method = HTTP_GET, .handler = h_get, .user_ctx = NULL,
};
static const httpd_uri_t s_post = {
    .uri = NEOPIXEL_URI, .method = HTTP_POST, .handler = h_post, .user_ctx = NULL,
};
static const httpd_uri_t s_options = {
    .uri = "/*", .method = HTTP_OPTIONS, .handler = h_options, .user_ctx = NULL,
};

esp_err_t http_strip_start(void) {
    if (s_server) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;   /* wildcard URI match */
    cfg.max_uri_handlers = 6;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 7;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    cfg.stack_size        = 8192;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }
    httpd_register_uri_handler(s_server, &s_get);
    httpd_register_uri_handler(s_server, &s_post);
    httpd_register_uri_handler(s_server, &s_options);
    ESP_LOGI(TAG, "neopixel API up: %s", NEOPIXEL_URI);
    return ESP_OK;
}

void http_strip_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
