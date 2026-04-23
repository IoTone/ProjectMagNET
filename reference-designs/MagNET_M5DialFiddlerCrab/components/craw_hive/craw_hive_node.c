/*
 * craw_hive_node — node side of the MagNET hive protocol.
 * Discovers the ruler via mDNS, authenticates with HMAC, maintains a
 * heartbeat session.
 */

#include "craw_hive.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "mdns.h"
#include "cJSON.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "craw_hive_node";

/* ---- Module state ---- */
typedef struct {
    craw_hive_node_config_t  cfg;
    TaskHandle_t             task;
    volatile bool            running;
    volatile craw_hive_node_state_t state;
    int                      sock;
    char                     session_id[40];
    uint32_t                 backoff_ms;
} node_ctx_t;

static node_ctx_t s_ctx = {0};

/* ---- Small helpers ---- */
static int64_t now_epoch_sec(void) {
    time_t t = time(NULL);
    return (int64_t)t;
}

static void set_state(craw_hive_node_state_t st, const char *info) {
    s_ctx.state = st;
    ESP_LOGI(TAG, "state=%d info=%s", (int)st, info ? info : "");
    if (s_ctx.cfg.on_state) s_ctx.cfg.on_state(st, info, s_ctx.cfg.on_state_ctx);
}

/* ---- mDNS discovery: find first _magnet-ruler._tcp whose TXT hive=<ours> ---- */
static bool discover_ruler(char *host_out, size_t host_out_len,
                           uint16_t *port_out) {
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(CRAW_HIVE_SERVICE_TYPE,
                                   CRAW_HIVE_SERVICE_PROTO,
                                   3000, 4, &results);
    if (err != ESP_OK || !results) {
        if (results) mdns_query_results_free(results);
        return false;
    }

    bool found = false;
    for (mdns_result_t *r = results; r && !found; r = r->next) {
        /* Check TXT: hive matches */
        bool hive_match = false, ver_ok = true;
        for (mdns_txt_item_t *t = r->txt; t && (int)(t - r->txt) < r->txt_count; t++) {
            if (!strcmp(t->key, "hive") && t->value &&
                s_ctx.cfg.hive_id && !strcmp(t->value, s_ctx.cfg.hive_id)) {
                hive_match = true;
            }
            if (!strcmp(t->key, "ver") && t->value) {
                if (atoi(t->value) > CRAW_HIVE_PROTO_VERSION) ver_ok = false;
            }
        }
        if (!hive_match || !ver_ok) continue;

        if (r->hostname) {
            /* mdns returns "hostname" without .local — add it. */
            snprintf(host_out, host_out_len, "%s.local", r->hostname);
            *port_out = r->port ? r->port : CRAW_HIVE_DEFAULT_PORT;
            found = true;
        }
    }
    mdns_query_results_free(results);
    return found;
}

/* ---- TCP I/O ---- */
static int tcp_connect(const char *host, uint16_t port) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "getaddrinfo failed for %s", host);
        return -1;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return -1; }
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "connect failed: errno=%d", errno);
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return sock;
}

static int send_all(int sock, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int recv_all(int sock, uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = recv(sock, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

/* Receive one length-prefixed frame into a heap buffer. */
static int recv_frame(int sock, uint8_t **frame_out, size_t *len_out) {
    uint8_t hdr[4];
    if (recv_all(sock, hdr, 4) != 0) return -1;
    uint32_t jlen = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16)
                  | ((uint32_t)hdr[2] <<  8) |  (uint32_t)hdr[3];
    if (jlen > CRAW_HIVE_MAX_FRAME) return -1;
    uint8_t *buf = malloc(4 + jlen);
    if (!buf) return -1;
    memcpy(buf, hdr, 4);
    if (recv_all(sock, buf + 4, jlen) != 0) { free(buf); return -1; }
    *frame_out = buf;
    *len_out = 4 + jlen;
    return 0;
}

/* ---- Protocol actions ---- */

static int send_msg(int sock, craw_hive_msg_type_t type, const char *to,
                    const char *payload_json) {
    craw_hive_msg_t m = {0};
    m.type = type;
    strncpy(m.from, s_ctx.cfg.node_id, CRAW_HIVE_ID_MAX);
    strncpy(m.to,   to,                CRAW_HIVE_ID_MAX);
    craw_hive_nonce_fill(m.nonce_hex);
    m.ts = now_epoch_sec();
    m.payload_json = payload_json ? strdup(payload_json) : NULL;
    uint8_t *frame = NULL; size_t flen = 0;
    int rc = craw_hive_proto_encode(&m, s_ctx.cfg.secret, CRAW_HIVE_SECRET_BYTES,
                                    &frame, &flen);
    craw_hive_msg_free(&m);
    if (rc != 0) return -1;
    rc = send_all(sock, frame, flen);
    free(frame);
    return rc;
}

static char *build_hello_payload(void) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "role_requested", s_ctx.cfg.role_requested);
    cJSON_AddStringToObject(p, "chip", s_ctx.cfg.chip ? s_ctx.cfg.chip : "unknown");
    cJSON_AddStringToObject(p, "fw",   s_ctx.cfg.fw   ? s_ctx.cfg.fw   : "0.0.0");
    cJSON_AddStringToObject(p, "hive", s_ctx.cfg.hive_id);
    cJSON *caps = cJSON_CreateArray();
    if (s_ctx.cfg.caps) {
        for (const char **c = s_ctx.cfg.caps; *c; c++) {
            cJSON_AddItemToArray(caps, cJSON_CreateString(*c));
        }
    }
    cJSON_AddItemToObject(p, "caps", caps);
    char *s = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    return s;
}

/* One join attempt: connect, HELLO, read WELCOME, then heartbeat loop.
 * Returns when session ends (for any reason). */
static void session_attempt(void) {
    char host[64]; uint16_t port = 0;
    set_state(CRAW_HIVE_NODE_DISCOVER, "mdns");
    if (!discover_ruler(host, sizeof(host), &port)) {
        set_state(CRAW_HIVE_NODE_BACKOFF, "no-ruler");
        return;
    }
    ESP_LOGI(TAG, "ruler %s:%u", host, port);

    set_state(CRAW_HIVE_NODE_CONNECTING, host);
    int sock = tcp_connect(host, port);
    if (sock < 0) {
        set_state(CRAW_HIVE_NODE_BACKOFF, "tcp");
        return;
    }
    s_ctx.sock = sock;

    char *hello = build_hello_payload();
    int rc = send_msg(sock, CRAW_HIVE_MSG_HELLO, "*", hello);
    free(hello);
    if (rc != 0) {
        close(sock); s_ctx.sock = -1;
        set_state(CRAW_HIVE_NODE_BACKOFF, "send-hello");
        return;
    }

    uint8_t *frame = NULL; size_t flen = 0;
    if (recv_frame(sock, &frame, &flen) != 0) {
        close(sock); s_ctx.sock = -1;
        set_state(CRAW_HIVE_NODE_BACKOFF, "recv-welcome");
        return;
    }
    craw_hive_msg_t in = {0};
    rc = craw_hive_proto_decode(frame, flen,
                                s_ctx.cfg.secret, CRAW_HIVE_SECRET_BYTES,
                                now_epoch_sec(), &in);
    free(frame);
    if (rc != 0 || in.type != CRAW_HIVE_MSG_WELCOME) {
        ESP_LOGW(TAG, "expected WELCOME, got type=%d rc=%d", (int)in.type, rc);
        craw_hive_msg_free(&in);
        close(sock); s_ctx.sock = -1;
        set_state(CRAW_HIVE_NODE_BACKOFF, "no-welcome");
        return;
    }

    /* Extract session_id */
    cJSON *pj = cJSON_Parse(in.payload_json);
    if (cJSON_IsObject(pj)) {
        const cJSON *sid = cJSON_GetObjectItemCaseSensitive(pj, "session_id");
        if (cJSON_IsString(sid)) {
            strncpy(s_ctx.session_id, sid->valuestring, sizeof(s_ctx.session_id) - 1);
        }
    }
    cJSON_Delete(pj);
    craw_hive_msg_free(&in);

    set_state(CRAW_HIVE_NODE_JOINED, s_ctx.session_id);
    s_ctx.backoff_ms = 0;

    /* Heartbeat loop. */
    int64_t last_ping = esp_timer_get_time() / 1000;
    while (s_ctx.running) {
        /* Non-blocking poll with small recv timeout (already SO_RCVTIMEO=5s). */
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        uint8_t hdr[4];
        int n = recv(sock, hdr, 4, 0);
        if (n == 4) {
            uint32_t jlen = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16)
                          | ((uint32_t)hdr[2] <<  8) |  (uint32_t)hdr[3];
            if (jlen > CRAW_HIVE_MAX_FRAME) break;
            uint8_t *buf = malloc(4 + jlen);
            memcpy(buf, hdr, 4);
            if (recv_all(sock, buf + 4, jlen) != 0) { free(buf); break; }
            /* Server messages (PING, ROLE_GRANT) — ignore payloads for now but
             * a valid frame keeps the session alive. */
            free(buf);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        /* Periodic PING. */
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - last_ping > CRAW_HIVE_HEARTBEAT_SEC * 1000) {
            if (send_msg(sock, CRAW_HIVE_MSG_PING, "*", NULL) != 0) break;
            last_ping = now_ms;
        }
    }
    close(sock); s_ctx.sock = -1;
    s_ctx.session_id[0] = '\0';
    set_state(CRAW_HIVE_NODE_BACKOFF, "session-end");
}

/* ---- Node task ---- */
static void node_task(void *arg) {
    (void)arg;
    s_ctx.backoff_ms = 0;
    while (s_ctx.running) {
        session_attempt();
        if (!s_ctx.running) break;
        /* Exponential-ish backoff, cap 120 s. */
        if (s_ctx.backoff_ms == 0) s_ctx.backoff_ms = 10 * 1000;
        else if (s_ctx.backoff_ms < 120 * 1000) s_ctx.backoff_ms *= 2;
        if (s_ctx.backoff_ms > 120 * 1000) s_ctx.backoff_ms = 120 * 1000;
        vTaskDelay(pdMS_TO_TICKS(s_ctx.backoff_ms));
    }
    s_ctx.task = NULL;
    vTaskDelete(NULL);
}

/* ---- Public ---- */

int craw_hive_node_start(const craw_hive_node_config_t *cfg) {
    if (!cfg || !cfg->node_id || !cfg->hive_id || !cfg->role_requested || !cfg->secret) {
        return -1;
    }
    if (s_ctx.running) return 0;
    s_ctx.cfg = *cfg;
    s_ctx.running = true;
    s_ctx.state = CRAW_HIVE_NODE_OFFLINE;
    s_ctx.sock = -1;
    /* mdns_init() must have been called elsewhere (or we call it). */
    mdns_init();
    if (xTaskCreate(node_task, "craw_hive_node", 6144, NULL, 5, &s_ctx.task) != pdPASS) {
        s_ctx.running = false;
        return -1;
    }
    return 0;
}

void craw_hive_node_stop(void) {
    s_ctx.running = false;
    if (s_ctx.sock >= 0) {
        shutdown(s_ctx.sock, SHUT_RDWR);
        close(s_ctx.sock);
        s_ctx.sock = -1;
    }
}

craw_hive_node_state_t craw_hive_node_state(void) {
    return s_ctx.state;
}

const char *craw_hive_node_session_id(void) {
    return s_ctx.session_id[0] ? s_ctx.session_id : NULL;
}
