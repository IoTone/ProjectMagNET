/*
 * http_stream.c — HTTP endpoints matching the stock ESP32 CameraWebServer
 * behavior, including its two-server split:
 *   :80   /          minimal HTML page with the MJPEG <img> + links
 *         /capture   single JPEG frame
 *         /control   ?var=X&val=Y sensor control (framesize, quality, ...)
 *         /status    JSON of current sensor state
 *         /stream    302 → :81/stream (compat shim for old URLs)
 *   :81   /stream    multipart/x-mixed-replace MJPEG
 *
 * The stream lives on its own httpd instance (stock CameraWebServer does the
 * same) because ESP-IDF httpd is a single task per instance and the MJPEG
 * handler never returns: with everything on one port, a single viewer wedges
 * /, /capture and /control for everyone — including page reloads, which then
 * look like a hang. On :81, lru_purge + max_open_sockets=2 means a new viewer
 * evicts a stale one instead of queueing behind it.
 *
 * MIT License, Copyright (c) 2026 IoTone, Inc.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "lwip/sockets.h"

#include "craw_camera.h"

static const char *TAG = "cam_http";

#define STREAM_PORT 81
/* Frame pacing: cap the MJPEG loop so it can't busy-spin capture+send and
 * starve the IDLE task (task-WDT reset) the way an unpaced loop on this
 * class of chip does. ~25 fps ceiling; actual rate is lower at VGA+ since
 * capture itself takes 30–100 ms. */
#define STREAM_FRAME_GAP_MS 40

static httpd_handle_t s_httpd        = NULL;   /* :80 control/UI server   */
static httpd_handle_t s_stream_httpd = NULL;   /* :81 MJPEG stream server */

/* ----- / ----- */
/* The page fetches :81/stream itself (URL built from location.hostname so it
 * works via IP and via <host>.local alike), parses the multipart stream in
 * JS, blits each JPEG to the <img>, and overlays live stats: rendered fps,
 * kB/frame, worst inter-frame gap, plus camera-side fps derived from the
 * X-Timestamp part header. Falls back to a raw-stream link if fetch fails. */
static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>MagNET Camera</title>"
    "<style>body{font-family:sans-serif;background:#111;color:#ddd;"
    "margin:0;padding:12px;}img{max-width:100%;border:1px solid #444;}"
    "#hud{font:13px monospace;color:#9f9;margin:6px 0;}"
    "a,button{color:#7cf;margin-right:12px;}</style></head><body>"
    "<h2>MagNET Hive Camera</h2>"
    "<p><img id=\"s\" alt=\"stream\"></p>"
    "<div id=\"hud\">connecting...</div>"
    "<p><a href=\"/capture\">/capture</a> "
    "<a href=\"/status\">/status</a> "
    "<a id=\"raw\">raw stream</a></p>"
    "<p>Controls: <code>/control?var=framesize&amp;val=8</code> "
    "(0=QQVGA, 5=VGA, 8=SVGA, 10=UXGA). "
    "<code>/control?var=quality&amp;val=10</code> (0–63, lower=better).</p>"
    "<script>\n"
    "var img=document.getElementById('s'),hud=document.getElementById('hud');\n"
    "var url=location.protocol+'//'+location.hostname+':81/stream';\n"
    "document.getElementById('raw').href=url;\n"
    "var times=[],camTimes=[],lastLen=0;\n"
    "function stats(){\n"
    " if(times.length<2)return;\n"
    " var mg=0;for(var i=1;i<times.length;i++)mg=Math.max(mg,times[i]-times[i-1]);\n"
    " var fps=1000*(times.length-1)/(times[times.length-1]-times[0]);\n"
    " var cam='';\n"
    " if(camTimes.length>1){var cf=(camTimes.length-1)/(camTimes[camTimes.length-1]-camTimes[0]);cam=' | cam '+cf.toFixed(1)+' fps';}\n"
    " hud.textContent=fps.toFixed(1)+' fps'+cam+' | '+(lastLen/1024).toFixed(1)+' kB/frame | max gap '+mg.toFixed(0)+' ms';\n"
    "}\n"
    "function push(a,v,n){a.push(v);while(a.length>n)a.shift();}\n"
    "fetch(url).then(function(r){\n"
    " var rd=r.body.getReader(),buf=new Uint8Array(0),dec=new TextDecoder();\n"
    " function cat(a,b){var c=new Uint8Array(a.length+b.length);c.set(a);c.set(b,a.length);return c;}\n"
    " function hdrEnd(h){for(var i=0;i<=h.length-4;i++){if(h[i]===13&&h[i+1]===10&&h[i+2]===13&&h[i+3]===10)return i;}return -1;}\n"
    " function pump(){rd.read().then(function(s){\n"
    "  if(s.done){hud.textContent='stream ended';return;}\n"
    "  buf=cat(buf,s.value);\n"
    "  for(;;){\n"
    "   var he=hdrEnd(buf);if(he<0)break;\n"
    "   var head=dec.decode(buf.subarray(0,he));\n"
    "   var m=head.match(/Content-Length: *(\\d+)/i);\n"
    "   if(!m){buf=buf.subarray(he+4);continue;}\n"
    "   var len=+m[1];if(buf.length<he+4+len)break;\n"
    "   var ts=head.match(/X-Timestamp: *([0-9.]+)/i);\n"
    "   var u=URL.createObjectURL(new Blob([buf.subarray(he+4,he+4+len)],{type:'image/jpeg'}));\n"
    "   img.onload=function(){URL.revokeObjectURL(this.src);};\n"
    "   img.src=u;lastLen=len;\n"
    "   push(times,performance.now(),31);\n"
    "   if(ts)push(camTimes,parseFloat(ts[1]),31);\n"
    "   stats();\n"
    "   buf=buf.subarray(he+4+len);\n"
    "  }\n"
    "  pump();\n"
    " });}\n"
    " pump();\n"
    "}).catch(function(e){hud.textContent='fetch failed: '+e+' - use the raw stream link';});\n"
    "</script>"
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
/* Boundary + part headers go out in ONE send (see Nagle note below).
 * X-Timestamp is the capture time (stock CameraWebServer field) so clients
 * can separate camera-side fps from network-side fps. */
static const char *STREAM_PART =
    "\r\n--" PART_BOUNDARY "\r\n"
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n"
    "X-Timestamp: %lld.%06ld\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
    /* SO_LINGER(0): when this socket closes — handler send-error, lru
     * eviction, server stop — abort the pcb with RST instead of lwIP's
     * default "drain queued data first". A vanished viewer (closed laptop,
     * killed tab) leaves a zero-recv-window socket with a full send queue;
     * lwIP's persist state never times that out, so each such corpse keeps
     * tcp_zero_window_probe/tcp_split_unsent_seg churning in tiT forever —
     * observed on-bench as IDLE1 task-WDT storms minutes after the viewer
     * died. RST-on-close frees the pcb and its queue immediately.
     *
     * TCP_NODELAY: the loop writes part-headers then the JPEG per frame.
     * With Nagle on, each small write after unACKed data stalls until the
     * client's delayed ACK (~40–100 ms) — capping the stream at ~10–25 fps
     * and adding stutter no matter how fast the sensor runs. */
    int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        struct linger lg = { .l_onoff = 1, .l_linger = 0 };
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        int nd = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
    }

    esp_err_t rc = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (rc != ESP_OK) return rc;
    set_cors(req);
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "stream: client connected (fd=%d)", fd);

    /* Per-window stats → serial every ~5 s: frames, fps×10, avg capture ms,
     * avg frame kB. Cheap integer math; this is the on-device half of the
     * FPS debugging story (the index page HUD is the browser half). */
    bool     first      = true;
    uint32_t nframes    = 0;
    uint32_t win_frames = 0;
    uint64_t win_bytes  = 0;
    int64_t  win_cap_us = 0;
    int64_t  win_t0     = t0;

    char part_buf[160];
    while (true) {
        int64_t tc = esp_timer_get_time();
        camera_fb_t *fb = craw_camera_capture();
        if (!fb) { rc = ESP_FAIL; break; }
        int64_t cap_us = esp_timer_get_time() - tc;
        size_t  len    = fb->len;

        int n = snprintf(part_buf, sizeof(part_buf), STREAM_PART, (unsigned)len,
                         (long long)fb->timestamp.tv_sec, fb->timestamp.tv_usec);
        rc = httpd_resp_send_chunk(req, part_buf, n);
        if (rc == ESP_OK) {
            rc = httpd_resp_send_chunk(req, (const char *)fb->buf, len);
        }
        craw_camera_release(fb);
        if (rc != ESP_OK) break;   /* client disconnected */

        nframes++; win_frames++; win_bytes += len; win_cap_us += cap_us;
        int64_t now = esp_timer_get_time();
        if (first) {
            first = false;
            ESP_LOGI(TAG, "stream: first frame at +%lld ms (capture %lld ms, %u B)",
                     (now - t0) / 1000, cap_us / 1000, (unsigned)len);
        }
        if (now - win_t0 >= 5000000) {
            int fps_x10 = (int)((int64_t)win_frames * 10000000 / (now - win_t0));
            ESP_LOGI(TAG, "stream: %d.%d fps, avg cap %lld ms, avg %llu kB/frame",
                     fps_x10 / 10, fps_x10 % 10,
                     win_cap_us / win_frames / 1000,
                     win_bytes / win_frames / 1024);
            win_frames = 0; win_bytes = 0; win_cap_us = 0; win_t0 = now;
        }
        vTaskDelay(pdMS_TO_TICKS(STREAM_FRAME_GAP_MS));
    }
    ESP_LOGI(TAG, "stream: client gone after %u frames", (unsigned)nframes);
    return rc;
}

/* :80/stream compat shim — anything still using the old single-port URL
 * (bookmarks, scripts, stream-url output from older builds) gets a 302 to
 * :81. Browsers follow redirects for <img>, curl needs -L. */
static esp_err_t stream_redirect_handler(httpd_req_t *req) {
    char host[64] = {0};
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    char *colon = strchr(host, ':');
    if (colon) *colon = '\0';            /* strip :80 if present */
    char loc[96];
    snprintf(loc, sizeof(loc), "http://%s:%d/stream",
             host[0] ? host : "", STREAM_PORT);
    set_cors(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
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

    /* Socket budget (CONFIG_LWIP_MAX_SOCKETS=16): each httpd instance holds
     * a listen socket + a UDP ctrl socket (4 total), control server caps at
     * 6 connections (Chrome's 2–3 speculative + /control + preflight),
     * stream server at 2 (active viewer + the incoming one that evicts it).
     * Worst case 12, leaving headroom for mDNS, SNTP and hive TCP. */
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = 80;
    cfg.ctrl_port         = 32768;
    cfg.max_open_sockets  = 6;
    cfg.max_uri_handlers  = 16;   /* currently ~5 in use; room to grow */
    cfg.stack_size        = 16384;
    cfg.lru_purge_enable  = true;  /* auto-close oldest idle socket */
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start (:80) failed");
        return -1;
    }
    httpd_uri_t u_index    = { .uri = "/",        .method = HTTP_GET, .handler = index_handler };
    httpd_uri_t u_redirect = { .uri = "/stream",  .method = HTTP_GET, .handler = stream_redirect_handler };
    httpd_uri_t u_capture  = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler };
    httpd_uri_t u_control  = { .uri = "/control", .method = HTTP_GET, .handler = control_handler };
    httpd_uri_t u_status   = { .uri = "/status",  .method = HTTP_GET, .handler = status_handler };
    httpd_register_uri_handler(s_httpd, &u_index);
    httpd_register_uri_handler(s_httpd, &u_redirect);
    httpd_register_uri_handler(s_httpd, &u_capture);
    httpd_register_uri_handler(s_httpd, &u_control);
    httpd_register_uri_handler(s_httpd, &u_status);

    /* Dedicated stream server (its own task) so a viewer parked on the
     * infinite MJPEG handler can't block /, /capture or /control. */
    httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
    scfg.server_port       = STREAM_PORT;
    scfg.ctrl_port         = 32769;   /* must differ from the :80 instance */
    scfg.max_open_sockets  = 2;
    scfg.max_uri_handlers  = 2;
    scfg.stack_size        = 8192;
    scfg.lru_purge_enable  = true;    /* new viewer evicts a stale stream */
    scfg.recv_wait_timeout = 5;
    scfg.send_wait_timeout = 5;

    if (httpd_start(&s_stream_httpd, &scfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start (:%d) failed", STREAM_PORT);
        httpd_stop(s_httpd);
        s_httpd = NULL;
        return -1;
    }
    httpd_uri_t u_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
    httpd_register_uri_handler(s_stream_httpd, &u_stream);

    ESP_LOGI(TAG, "camera HTTP: :80 / /capture /control /status, :%d /stream",
             STREAM_PORT);
    return 0;
}

void cam_http_stop(void) {
    if (s_stream_httpd) { httpd_stop(s_stream_httpd); s_stream_httpd = NULL; }
    if (s_httpd)        { httpd_stop(s_httpd);        s_httpd = NULL; }
}
