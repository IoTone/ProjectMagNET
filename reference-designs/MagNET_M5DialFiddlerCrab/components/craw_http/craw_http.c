#include "craw_http.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"

static const char *TAG = "craw_http";

static httpd_handle_t s_server = NULL;

static craw_http_notify_cb_t s_notify_cb = NULL;
static craw_http_status_cb_t s_status_cb = NULL;
static void *s_ctx = NULL;

// ---------------------------------------------------------------------------
// /notify — parse query params into craw_mqtt_msg_t and invoke callback
// ---------------------------------------------------------------------------
static esp_err_t handler_notify(httpd_req_t *req) {
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        craw_mqtt_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.session_pct = -1;
        msg.weekly_pct  = -1;

        char param[64] = {0};
        if (httpd_query_key_value(query, "state", param, sizeof(param)) == ESP_OK) {
            msg.state = atoi(param);
        }
        if (httpd_query_key_value(query, "model", param, sizeof(param)) == ESP_OK) {
            strncpy(msg.model, param, sizeof(msg.model) - 1);
            msg.model[sizeof(msg.model) - 1] = '\0';
        }
        if (httpd_query_key_value(query, "session", param, sizeof(param)) == ESP_OK) {
            msg.session_pct = atoi(param);
        }
        if (httpd_query_key_value(query, "weekly", param, sizeof(param)) == ESP_OK) {
            msg.weekly_pct = atoi(param);
        }
        if (httpd_query_key_value(query, "host", param, sizeof(param)) == ESP_OK) {
            strncpy(msg.client_host, param, sizeof(msg.client_host) - 1);
            msg.client_host[sizeof(msg.client_host) - 1] = '\0';
        }

        ESP_LOGI(TAG, "notify: state=%d model=%s sess=%d wkly=%d",
                 msg.state, msg.model, msg.session_pct, msg.weekly_pct);

        if (s_notify_cb) s_notify_cb(&msg, s_ctx);
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// /status — delegate JSON formatting to the application via callback
// ---------------------------------------------------------------------------
static esp_err_t handler_status(httpd_req_t *req) {
    const char *json = NULL;
    if (s_status_cb) {
        json = s_status_cb(s_ctx);
    }

    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
    } else {
        httpd_resp_sendstr(req, "{\"error\":\"no status available\"}");
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// /ping — simple health check
// ---------------------------------------------------------------------------
static esp_err_t handler_ping(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "pong");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void craw_http_start(craw_http_notify_cb_t notify_cb,
                     craw_http_status_cb_t status_cb, void *ctx) {
    if (s_server) return;

    s_notify_cb = notify_cb;
    s_status_cb = status_cb;
    s_ctx       = ctx;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;

    if (httpd_start(&s_server, &config) == ESP_OK) {
        httpd_uri_t uri_notify = {
            .uri = "/notify", .method = HTTP_GET,
            .handler = handler_notify, .user_ctx = NULL
        };
        httpd_uri_t uri_status = {
            .uri = "/status", .method = HTTP_GET,
            .handler = handler_status, .user_ctx = NULL
        };
        httpd_uri_t uri_ping = {
            .uri = "/ping", .method = HTTP_GET,
            .handler = handler_ping, .user_ctx = NULL
        };
        httpd_register_uri_handler(s_server, &uri_notify);
        httpd_register_uri_handler(s_server, &uri_status);
        httpd_register_uri_handler(s_server, &uri_ping);
        ESP_LOGI(TAG, "HTTP server started on port 80");
    }
}

void craw_http_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
