/*
 * http_stream.c — HTTP endpoints matching the stock ESP32 CameraWebServer
 * behavior. Ports:
 *   /         minimal HTML page with <img src="/stream"> + link to /capture
 *   /stream   multipart/x-mixed-replace MJPEG (same format browsers expect)
 *   /capture  single JPEG frame
 *   /control  ?var=X&val=Y sensor control (framesize, quality, brightness...)
 *   /status   JSON of current sensor state + hive info
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_camera.h"

#include "craw_camera.h"

static const char *TAG = "cam_http";

static httpd_handle_t s_httpd = NULL;

/* ----- / ----- */
static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>MagNET Camera</title>"
    "<style>body{font-family:sans-serif;background:#111;color:#ddd;"
    "margin:0;padding:12px;}img{max-width:100%;border:1px solid #444;}"
    "a,button{color:#7cf;margin-right:12px;}</style></head><body>"
    "<h2>MagNET Hive Camera</h2>"
    "<p><img src=\"/stream\" alt=\"stream\"></p>"
    "<p><a href=\"/capture\">/capture</a> "
    "<a href=\"/status\">/status</a></p>"
    "<p>Controls: <code>/control?var=framesize&amp;val=8</code> "
    "(0=QQVGA, 5=VGA, 8=SVGA, 10=UXGA). "
    "<code>/control?var=quality&amp;val=10</code> (0–63, lower=better).</p>"
    "</body></html>";

/* Every endpoint emits permissive CORS headers so browser-hosted viewers
 * on any origin (localhost dev pages, file://, other LAN devices) can
 * read images and sensor state. Access-Control-Allow-Methods advertises
 * GET, OPTIONS so preflight-strict clients don't 405. */
static void set_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
}

static esp_err_t index_handler(httpd_req_t *req) {
    set_cors(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, INDEX_HTML);
}

/* ----- /capture ----- */
static esp_err_t capture_handler(httpd_req_t *req) {
    set_cors(req);
    camera_fb_t *fb = craw_camera_capture();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture fail");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    esp_err_t rc = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    craw_camera_release(fb);
    return rc;
}

/* ----- /stream (multipart MJPEG) ----- */
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART     =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
    esp_err_t rc = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (rc != ESP_OK) return rc;
    set_cors(req);
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    char part_buf[64];
    while (true) {
        camera_fb_t *fb = craw_camera_capture();
        if (!fb) { rc = ESP_FAIL; break; }

        rc = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (rc == ESP_OK) {
            int n = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
            rc = httpd_resp_send_chunk(req, part_buf, n);
        }
        if (rc == ESP_OK) {
            rc = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        craw_camera_release(fb);
        if (rc != ESP_OK) break;   /* client disconnected */
    }
    return rc;
}

/* ----- /control ----- */
static esp_err_t control_handler(httpd_req_t *req) {
    set_cors(req);
    char query[96] = {0};
    char var[24]   = {0};
    char val[24]   = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no query");
        return ESP_FAIL;
    }
    httpd_query_key_value(query, "var", var, sizeof(var));
    httpd_query_key_value(query, "val", val, sizeof(val));
    int v = atoi(val);

    sensor_t *s = esp_camera_sensor_get();
    int rc = -1;
    if      (!strcmp(var, "framesize"))  rc = craw_camera_set_framesize(v);
    else if (!strcmp(var, "quality"))    rc = craw_camera_set_quality(v);
    else if (s && !strcmp(var, "brightness") && s->set_brightness) rc = s->set_brightness(s, v);
    else if (s && !strcmp(var, "contrast")   && s->set_contrast)   rc = s->set_contrast(s, v);
    else if (s && !strcmp(var, "saturation") && s->set_saturation) rc = s->set_saturation(s, v);
    else if (s && !strcmp(var, "hmirror")    && s->set_hmirror)    rc = s->set_hmirror(s, v);
    else if (s && !strcmp(var, "vflip")      && s->set_vflip)      rc = s->set_vflip(s, v);
    else if (!strcmp(var, "flash"))         { if (v) craw_camera_flash_on(); else craw_camera_flash_off(); rc = 0; }
    else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown var");
        return ESP_FAIL;
    }

    if (rc != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "control fail");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ----- /status ----- */
static esp_err_t status_handler(httpd_req_t *req) {
    set_cors(req);
    sensor_t *s = esp_camera_sensor_get();
    char buf[384];
    int framesize = s ? s->status.framesize : -1;
    int quality   = s ? s->status.quality   : -1;
    snprintf(buf, sizeof(buf),
        "{\"board\":\"%s\",\"framesize\":%d,\"quality\":%d,"
        "\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,"
        "\"hmirror\":%d,\"vflip\":%d,\"flash_gpio\":%d}",
        craw_camera_board_name(),
        framesize, quality,
        s ? s->status.brightness : 0,
        s ? s->status.contrast   : 0,
        s ? s->status.saturation : 0,
        s ? s->status.hmirror    : 0,
        s ? s->status.vflip      : 0,
        craw_camera_flash_gpio());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* ----- Lifecycle ----- */
int cam_http_start(void) {
    if (s_httpd) return 0;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = 80;
    cfg.ctrl_port         = 32768;
    /* 13 slots: Chrome opens 2–3 speculative connections per host, MJPEG
     * /stream holds a slot per active viewer for the life of the stream,
     * and we want headroom for OPTIONS preflights + concurrent /control
     * requests from a webapp. lru_purge_enable auto-closes the oldest
     * idle socket when a new one arrives → immune to browser socket leaks. */
    cfg.max_open_sockets  = 13;
    cfg.max_uri_handlers  = 16;   /* currently ~5 in use; room to grow */
    cfg.stack_size        = 16384;
    cfg.lru_purge_enable  = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return -1;
    }
    httpd_uri_t u_index   = { .uri = "/",         .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t u_stream  = { .uri = "/stream",   .method = HTTP_GET, .handler = stream_handler };
    httpd_uri_t u_capture = { .uri = "/capture",  .method = HTTP_GET, .handler = capture_handler };
    httpd_uri_t u_control = { .uri = "/control",  .method = HTTP_GET, .handler = control_handler };
    httpd_uri_t u_status  = { .uri = "/status",   .method = HTTP_GET, .handler = status_handler };
    httpd_register_uri_handler(s_httpd, &u_index);
    httpd_register_uri_handler(s_httpd, &u_stream);
    httpd_register_uri_handler(s_httpd, &u_capture);
    httpd_register_uri_handler(s_httpd, &u_control);
    httpd_register_uri_handler(s_httpd, &u_status);
    ESP_LOGI(TAG, "camera HTTP server on :80  /stream /capture /control /status");
    return 0;
}

void cam_http_stop(void) {
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
}
