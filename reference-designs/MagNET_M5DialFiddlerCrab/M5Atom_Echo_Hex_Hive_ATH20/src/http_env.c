/*
 * http_env.c - UC2 environment sensor HTTP API
 *
 * Mirrors http_strip.c (lighting) / http_speaker.c (boombox) for the
 * single-node-per-actuator pattern: CORS-permissive JSON over plain
 * HTTP, wildcard OPTIONS preflight, cJSON for response framing.
 */

#include <string.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"
#include "http_env.h"
#include "env_state.h"

static const char *TAG = "http_env";
static httpd_handle_t s_server = NULL;

#define ENV_URI  "/api/v1/sensor/environment"
#define TEMP_URI "/api/v1/sensor/temperature"
#define HUM_URI  "/api/v1/sensor/humidity"

static void cors_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
}

static esp_err_t h_env(httpd_req_t *req) {
    env_reading_t r; env_state_get(&r);
    float t_off, h_off; env_get_cal(&t_off, &h_off);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject  (root, "valid", r.valid);
    cJSON_AddNumberToObject(root, "temperature_c", r.t_c);
    cJSON_AddNumberToObject(root, "humidity_pct",  r.rh_pct);
    cJSON_AddNumberToObject(root, "ts_ms",   (double)(r.ts_us / 1000));
    cJSON_AddNumberToObject(root, "timestamp_us", (double)esp_timer_get_time());
    cJSON *cal = cJSON_AddObjectToObject(root, "calibration");
    cJSON_AddNumberToObject(cal, "t_offset_c",  t_off);
    cJSON_AddNumberToObject(cal, "rh_offset_pct", h_off);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, body ? body : "{}");
    if (body) cJSON_free(body);
    return e;
}

static esp_err_t h_temp(httpd_req_t *req) {
    env_reading_t r; env_state_get(&r);
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "{\"valid\":%s,\"value_c\":%.2f,\"ts_ms\":%lld}",
        r.valid ? "true" : "false", r.t_c, (long long)(r.ts_us / 1000));
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t h_hum(httpd_req_t *req) {
    env_reading_t r; env_state_get(&r);
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "{\"valid\":%s,\"value_pct\":%.2f,\"ts_ms\":%lld}",
        r.valid ? "true" : "false", r.rh_pct, (long long)(r.ts_us / 1000));
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t h_options(httpd_req_t *req) {
    cors_headers(req);
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t s_get_env  = { .uri = ENV_URI,  .method = HTTP_GET, .handler = h_env,  .user_ctx = NULL };
static const httpd_uri_t s_get_temp = { .uri = TEMP_URI, .method = HTTP_GET, .handler = h_temp, .user_ctx = NULL };
static const httpd_uri_t s_get_hum  = { .uri = HUM_URI,  .method = HTTP_GET, .handler = h_hum,  .user_ctx = NULL };
static const httpd_uri_t s_options  = { .uri = "/*", .method = HTTP_OPTIONS, .handler = h_options, .user_ctx = NULL };

esp_err_t http_env_start(void) {
    if (s_server) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
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
    httpd_register_uri_handler(s_server, &s_get_env);
    httpd_register_uri_handler(s_server, &s_get_temp);
    httpd_register_uri_handler(s_server, &s_get_hum);
    httpd_register_uri_handler(s_server, &s_options);
    ESP_LOGI(TAG, "env API up: %s + %s + %s", ENV_URI, TEMP_URI, HUM_URI);
    return ESP_OK;
}

void http_env_stop(void) {
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
}
