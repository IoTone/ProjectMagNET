/*
 * http_speaker.c - UC2 speaker actuator HTTP API
 *
 * Contract (matches the in-XR UC2 actuator panel + mock-join-server):
 *   POST /api/v1/actuator/speaker/play
 *     body: { "sound_id": "<name>" }
 *     response: { "played": "<canonical-name>", "at": <epoch-ms> }
 *   GET /api/v1/actuator/speaker
 *     response: { "sounds": [...], "volume": 0..100, "amp_on": bool, ... }
 *   OPTIONS (wildcard)
 *     CORS preflight
 *
 * Sound dispatch maps panel-friendly names to the craw_audio_play_*
 * library. UC2's Chime + Doorbell buttons send sound_id="chime" /
 * "doorbell"; the rest of the catalog is accepted as a passthrough so
 * the same endpoint covers Forth-side sounds without a panel-side
 * vocabulary change.
 */

#include <string.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"
#include "craw_audio.h"
#include "http_speaker.h"

static const char *TAG = "http_speaker";
static httpd_handle_t s_server = NULL;

#define BASE_URI "/api/v1/actuator/speaker"
#define PLAY_URI BASE_URI "/play"

static void cors_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
}

/* Map a panel-friendly sound_id onto a craw_audio function. Returns the
 * canonical name written back to the client (so the panel knows what
 * actually played — e.g. "chime" played as "notify"), or NULL on miss. */
static const char *play_sound(const char *id) {
    if (!id || !*id) return NULL;
    /* UC2 panel-friendly aliases */
    if (!strcmp(id, "chime"))    { craw_audio_play_notify();   return "notify";   }
    if (!strcmp(id, "doorbell")) { craw_audio_play_alert();    return "alert";    }
    /* Native craw_audio names — accepted as passthrough */
    if (!strcmp(id, "alert"))    { craw_audio_play_alert();    return "alert";    }
    if (!strcmp(id, "notify"))   { craw_audio_play_notify();   return "notify";   }
    if (!strcmp(id, "warn"))     { craw_audio_play_warn();     return "warn";     }
    if (!strcmp(id, "error"))    { craw_audio_play_error();    return "error";    }
    if (!strcmp(id, "sunrise"))  { craw_audio_play_sunrise();  return "sunrise";  }
    if (!strcmp(id, "siren"))    { craw_audio_play_siren();    return "siren";    }
    if (!strcmp(id, "yelp"))     { craw_audio_play_yelp();     return "yelp";     }
    if (!strcmp(id, "nee-naw"))  { craw_audio_play_nee_naw();  return "nee-naw";  }
    if (!strcmp(id, "air-raid")) { craw_audio_play_air_raid(); return "air-raid"; }
    return NULL;
}

/* Sound catalog returned by GET. Keep in lockstep with play_sound(). */
static const char *const SOUND_CATALOG[] = {
    "chime", "doorbell",
    "alert", "notify", "warn", "error", "sunrise",
    "siren", "yelp", "nee-naw", "air-raid",
};
#define SOUND_CATALOG_COUNT (sizeof(SOUND_CATALOG) / sizeof(SOUND_CATALOG[0]))

static esp_err_t h_get(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON *sounds = cJSON_AddArrayToObject(root, "sounds");
    for (size_t i = 0; i < SOUND_CATALOG_COUNT; i++)
        cJSON_AddItemToArray(sounds, cJSON_CreateString(SOUND_CATALOG[i]));
    cJSON_AddNumberToObject(root, "volume", craw_audio_volume_get());
    cJSON_AddNumberToObject(root, "timestamp_us",
                            (double)esp_timer_get_time());
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, body ? body : "{}");
    if (body) cJSON_free(body);
    return e;
}

static esp_err_t h_post_play(httpd_req_t *req) {
    char buf[256];
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
    cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "sound_id");
    const char *id = (cJSON_IsString(jid) && jid->valuestring) ? jid->valuestring : NULL;
    const char *played = play_sound(id);
    cJSON_Delete(root);

    if (!played) {
        httpd_resp_set_status(req, "400 Bad Request");
        cors_headers(req);
        httpd_resp_sendstr(req, "{\"error\":\"unknown sound_id\"}");
        return ESP_OK;
    }

    /* Mirror mock-join-server: {played, at:<epoch-ms>} */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "played", played);
    cJSON_AddNumberToObject(resp, "at",
                            (double)(esp_timer_get_time() / 1000));
    char *body = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, body ? body : "{}");
    if (body) cJSON_free(body);
    return e;
}

static esp_err_t h_options(httpd_req_t *req) {
    cors_headers(req);
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t s_get_root = {
    .uri = BASE_URI, .method = HTTP_GET, .handler = h_get, .user_ctx = NULL,
};
static const httpd_uri_t s_post_play = {
    .uri = PLAY_URI, .method = HTTP_POST, .handler = h_post_play, .user_ctx = NULL,
};
static const httpd_uri_t s_options = {
    .uri = "/*", .method = HTTP_OPTIONS, .handler = h_options, .user_ctx = NULL,
};

esp_err_t http_speaker_start(void) {
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
    httpd_register_uri_handler(s_server, &s_get_root);
    httpd_register_uri_handler(s_server, &s_post_play);
    httpd_register_uri_handler(s_server, &s_options);
    ESP_LOGI(TAG, "speaker API up: %s + %s", BASE_URI, PLAY_URI);
    return ESP_OK;
}

void http_speaker_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
