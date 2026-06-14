/* craw_imu_http — esp_http_server wrapper around the IMU snapshot.
 *
 * Exposes a single endpoint:
 *
 *   GET /api/v1/sensor/imu  →  JSON snapshot matching the shape the
 *                              UC4 airplane-imu mark already polls
 *                              (orientation / angular_velocity /
 *                              acceleration / timestamp_us).
 *
 * CORS is permissive (`Access-Control-Allow-Origin: *`) so the
 * d3-spatial dataspace can fetch directly when running behind a
 * cloudflared tunnel — same pattern as the AHT20 env sensor and the
 * vitals device. The Vite proxy still strips request headers the
 * device httpd's 1024-byte header buffer can't hold.
 *
 * Server lifecycle is paired with `imu-on`/`imu-off`: we don't run
 * the listener at boot. The Forth surface in main.c calls
 * craw_imu_http_start() / craw_imu_http_stop() so the user controls
 * when the device is reachable on the LAN.
 */

#include "craw_imu.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"

static const char *TAG = "craw_imu_http";

static httpd_handle_t s_httpd = NULL;

/* Format the snapshot as the JSON shape UC4 polls. Buffer sizing:
 *  - 11 fields × ~22 chars worst-case (negative double-digit float with
 *    6 decimals) + 80 chars of structure = ~320 chars. 512 is plenty. */
static esp_err_t handle_imu_get(httpd_req_t *req)
{
    craw_imu_snapshot_t s;
    craw_imu_snapshot(&s);

    char body[512];
    int n = snprintf(body, sizeof(body),
        "{"
          "\"orientation\":{"
            "\"roll_rad\":%.6f,\"pitch_rad\":%.6f,\"yaw_rad\":%.6f"
          "},"
          "\"angular_velocity\":{"
            "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f"
          "},"
          "\"acceleration\":{"
            "\"x\":%.6f,\"y\":%.6f,\"z\":%.6f"
          "},"
          "\"timestamp_us\":%llu"
        "}",
        s.roll_rad,  s.pitch_rad, s.yaw_rad,
        s.gyro_x,    s.gyro_y,    s.gyro_z,
        s.accel_x,   s.accel_y,   s.accel_z,
        (unsigned long long)s.timestamp_us);

    if (n < 0 || n >= (int)sizeof(body)) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    /* CORS — see comment at top of file. */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    /* Cache-Control:no-cache so a polling client doesn't get a stale
     * 200-from-cache when re-loading the dataspace through a CDN. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, body, n);
}

/* OPTIONS preflight — same CORS headers, empty body. The Vite proxy
 * typically handles preflight itself, but on direct LAN access from
 * an XR browser (no proxy) we still need it. */
static esp_err_t handle_imu_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t craw_imu_http_start(void)
{
    if (s_httpd) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    /* Single-client behaviour matches the rest of the MagNET ESP-IDF
     * httpd surface — see [project_esp_httpd_serialize_via_proxy].
     * Vite proxy uses maxSockets:1 so we don't need lru_purge tricks. */
    cfg.max_open_sockets   = 4;
    cfg.max_uri_handlers   = 4;
    cfg.lru_purge_enable   = true;
    cfg.recv_wait_timeout  = 5;
    cfg.send_wait_timeout  = 5;
    /* Browser header noise (sec-ch-*, cookie, referer) easily exceeds
     * the default 512-byte cap; the Vite proxy strips most of it but
     * keep some headroom for direct-LAN access. */
    cfg.uri_match_fn       = httpd_uri_match_wildcard;

    esp_err_t rc = httpd_start(&s_httpd, &cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %d", rc);
        s_httpd = NULL;
        return rc;
    }

    httpd_uri_t get_uri = {
        .uri      = "/api/v1/sensor/imu",
        .method   = HTTP_GET,
        .handler  = handle_imu_get,
        .user_ctx = NULL,
    };
    httpd_uri_t opt_uri = {
        .uri      = "/api/v1/sensor/imu",
        .method   = HTTP_OPTIONS,
        .handler  = handle_imu_options,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &get_uri);
    httpd_register_uri_handler(s_httpd, &opt_uri);

    ESP_LOGI(TAG, "httpd up on :80 (GET /api/v1/sensor/imu)");
    return ESP_OK;
}

esp_err_t craw_imu_http_stop(void)
{
    if (!s_httpd) return ESP_OK;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    ESP_LOGI(TAG, "httpd stopped");
    return ESP_OK;
}

bool craw_imu_http_running(void) { return s_httpd != NULL; }
