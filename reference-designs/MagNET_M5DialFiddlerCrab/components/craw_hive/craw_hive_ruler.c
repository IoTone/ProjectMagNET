/*
 * craw_hive_ruler — ruler side of the MagNET hive protocol.
 * Advertises via mDNS, listens on a TCP port, validates HMAC on incoming
 * HELLO, auto-accepts (consensus stub) and issues a session_id.
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
#include "esp_random.h"
#include "esp_timer.h"
#include "mdns.h"
#include "cJSON.h"

#include "lwip/sockets.h"

static const char *TAG = "craw_hive_ruler";

#define MAX_SESSIONS 8

typedef struct {
    char     node_id[CRAW_HIVE_ID_MAX + 1];
    char     role[CRAW_HIVE_ROLE_MAX + 1];
    char     session_id[40];
    int64_t  last_seen_ms;
    bool     in_use;
} session_t;

static session_t s_sessions[MAX_SESSIONS];

typedef struct {
    craw_hive_ruler_config_t cfg;
    TaskHandle_t             task;
    volatile bool            running;
    int                      listen_sock;
} ruler_ctx_t;

static ruler_ctx_t s_ctx = {0};

/* ---- Helpers ---- */
static int64_t now_epoch_sec(void) { return (int64_t)time(NULL); }

static void gen_session_id(char *out) {
    uint8_t raw[16];
    for (size_t i = 0; i < sizeof(raw); i += 4) {
        uint32_t r = esp_random();
        memcpy(raw + i, &r, 4);
    }
    static const char H[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = H[(raw[i] >> 4) & 0xF];
        out[o++] = H[raw[i] & 0xF];
    }
    out[o] = '\0';
}

static session_t *session_for(const char *node_id) {
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (s_sessions[i].in_use && !strcmp(s_sessions[i].node_id, node_id))
            return &s_sessions[i];
    return NULL;
}

static session_t *session_alloc(void) {
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!s_sessions[i].in_use) return &s_sessions[i];
    return NULL;
}

/* ---- Framing (shares impl with node; kept small so no shared helper file yet) ---- */
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

static int send_msg(int sock, craw_hive_msg_type_t type, const char *to,
                    const char *payload_json) {
    craw_hive_msg_t m = {0};
    m.type = type;
    strncpy(m.from, s_ctx.cfg.ruler_id, CRAW_HIVE_ID_MAX);
    strncpy(m.to,   to,                 CRAW_HIVE_ID_MAX);
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

/* ---- HELLO handler ---- */
static void handle_hello(int client_sock, const craw_hive_msg_t *hello) {
    /* Parse payload */
    cJSON *p = cJSON_Parse(hello->payload_json ? hello->payload_json : "{}");
    const cJSON *role_req = cJSON_GetObjectItemCaseSensitive(p, "role_requested");
    const cJSON *hive     = cJSON_GetObjectItemCaseSensitive(p, "hive");

    if (!cJSON_IsString(role_req) || !cJSON_IsString(hive) ||
        !s_ctx.cfg.hive_id || strcmp(hive->valuestring, s_ctx.cfg.hive_id) != 0) {
        cJSON *rp = cJSON_CreateObject();
        cJSON_AddStringToObject(rp, "reason", "hive_mismatch");
        char *rps = cJSON_PrintUnformatted(rp);
        send_msg(client_sock, CRAW_HIVE_MSG_REJECT, hello->from, rps);
        free(rps); cJSON_Delete(rp); cJSON_Delete(p);
        return;
    }

    /* Consensus stub: accept unless the role is weird or we're full. */
    bool accept = true;
    char role_out[CRAW_HIVE_ROLE_MAX + 1] = {0};
    strncpy(role_out, role_req->valuestring, CRAW_HIVE_ROLE_MAX);

    if (s_ctx.cfg.on_hello) {
        s_ctx.cfg.on_hello(hello->from, role_req->valuestring, hive->valuestring,
                           &accept, role_out, sizeof(role_out), s_ctx.cfg.on_hello_ctx);
    }

    session_t *s = session_for(hello->from);
    if (!s) s = session_alloc();
    if (!s) accept = false;

    if (!accept) {
        cJSON *rp = cJSON_CreateObject();
        cJSON_AddStringToObject(rp, "reason", s ? "rejected" : "full");
        char *rps = cJSON_PrintUnformatted(rp);
        send_msg(client_sock, CRAW_HIVE_MSG_REJECT, hello->from, rps);
        free(rps); cJSON_Delete(rp); cJSON_Delete(p);
        return;
    }

    /* Populate session */
    s->in_use = true;
    strncpy(s->node_id, hello->from, CRAW_HIVE_ID_MAX);
    strncpy(s->role, role_out, CRAW_HIVE_ROLE_MAX);
    gen_session_id(s->session_id);

    /* WELCOME */
    cJSON *rp = cJSON_CreateObject();
    cJSON_AddStringToObject(rp, "session_id", s->session_id);
    cJSON_AddStringToObject(rp, "role",       s->role);
    cJSON_AddNumberToObject(rp, "heartbeat",  CRAW_HIVE_HEARTBEAT_SEC);
    char *rps = cJSON_PrintUnformatted(rp);
    send_msg(client_sock, CRAW_HIVE_MSG_WELCOME, hello->from, rps);
    ESP_LOGI(TAG, "WELCOME node=%s role=%s session=%s",
             s->node_id, s->role, s->session_id);
    free(rps); cJSON_Delete(rp); cJSON_Delete(p);
}

/* ---- Per-client handler (one connection → one session lifecycle) ---- */
static void handle_client(int client_sock) {
    /* Bound the initial HELLO read so a stuck client doesn't hang the
     * listener task (which is single-threaded). */
    struct timeval rcv_tv = { .tv_sec = 8, .tv_usec = 0 };
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));

    /* Read HELLO */
    uint8_t *frame = NULL; size_t flen = 0;
    int rfrc = recv_frame(client_sock, &frame, &flen);
    if (rfrc != 0) {
        ESP_LOGW(TAG, "recv HELLO frame failed rc=%d errno=%d", rfrc, errno);
        return;
    }
    ESP_LOGI(TAG, "rx HELLO frame %u bytes — decoding...", (unsigned)flen);

    craw_hive_msg_t in = {0};
    int64_t decode_t0 = esp_timer_get_time();
    int rc = craw_hive_proto_decode(frame, flen,
                                    s_ctx.cfg.secret, CRAW_HIVE_SECRET_BYTES,
                                    now_epoch_sec(), &in);
    int64_t decode_us = esp_timer_get_time() - decode_t0;
    ESP_LOGI(TAG, "decode rc=%d (%lld us)", rc, decode_us);
    free(frame);
    if (rc != 0) {
        ESP_LOGW(TAG, "HELLO decode failed rc=%d (1=auth, 3=ts_skew, 4=replay)", rc);
        cJSON *rp = cJSON_CreateObject();
        cJSON_AddStringToObject(rp, "reason", "auth");
        char *rps = cJSON_PrintUnformatted(rp);
        int src = send_msg(client_sock, CRAW_HIVE_MSG_REJECT, "*", rps);
        ESP_LOGW(TAG, "tx REJECT rc=%d", src);
        free(rps); cJSON_Delete(rp);
        return;
    }
    if (in.type != CRAW_HIVE_MSG_HELLO) {
        ESP_LOGW(TAG, "first frame was type=%d, expected HELLO", (int)in.type);
        craw_hive_msg_free(&in);
        return;
    }

    handle_hello(client_sock, &in);
    craw_hive_msg_free(&in);

    /* Echo / heartbeat loop — keep connection open until peer disconnects. */
    struct timeval tv = { .tv_sec = (CRAW_HIVE_HEARTBEAT_SEC * 3), .tv_usec = 0 };
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (1) {
        if (recv_frame(client_sock, &frame, &flen) != 0) break;
        craw_hive_msg_t m = {0};
        if (craw_hive_proto_decode(frame, flen,
                                   s_ctx.cfg.secret, CRAW_HIVE_SECRET_BYTES,
                                   now_epoch_sec(), &m) == 0) {
            session_t *s = session_for(m.from);
            if (s) s->last_seen_ms = esp_timer_get_time() / 1000;
            if (m.type == CRAW_HIVE_MSG_PING) {
                send_msg(client_sock, CRAW_HIVE_MSG_PING, m.from, NULL);
            }
            craw_hive_msg_free(&m);
        }
        free(frame);
    }
}

/* ---- Per-client task wrapper ----
 * Each accepted connection runs handle_client on its own FreeRTOS task so
 * the listener can return to accept() immediately. Without this, only one
 * peer at a time can hold a session — every other peer's SYN gets RST. */
typedef struct {
    int  sock;
    char peer_ip[INET_ADDRSTRLEN];
    int  peer_port;
} client_ctx_t;

static void client_task(void *arg) {
    client_ctx_t *cc = (client_ctx_t *)arg;
    handle_client(cc->sock);
    close(cc->sock);
    ESP_LOGI(TAG, "client %s:%d closed", cc->peer_ip, cc->peer_port);
    free(cc);
    vTaskDelete(NULL);
}

/* ---- Accept loop ---- */
static void listener_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        s_ctx.task = NULL;
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(s_ctx.cfg.port ? s_ctx.cfg.port : CRAW_HIVE_DEFAULT_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind failed errno=%d", errno);
        close(sock);
        s_ctx.task = NULL;
        vTaskDelete(NULL);
        return;
    }
    listen(sock, 4);
    s_ctx.listen_sock = sock;
    ESP_LOGI(TAG, "listening on port %u", (unsigned)ntohs(addr.sin_port));

    while (s_ctx.running) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int c = accept(sock, (struct sockaddr *)&peer, &peer_len);
        if (c < 0) {
            if (!s_ctx.running) break;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        char ipstr[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ipstr, sizeof(ipstr));
        ESP_LOGI(TAG, "accept from %s:%u (sock=%d)",
                 ipstr, (unsigned)ntohs(peer.sin_port), c);

        /* Hand the connection off to a per-client task so the listener
         * stays available for new accepts. The previous code blocked the
         * accept loop for the lifetime of the heartbeat session, locking
         * out all subsequent peers (kernel backlog overflowed → RST). */
        client_ctx_t *cc = malloc(sizeof(*cc));
        if (!cc) {
            ESP_LOGE(TAG, "client ctx malloc failed");
            close(c);
            continue;
        }
        cc->sock = c;
        strncpy(cc->peer_ip, ipstr, sizeof(cc->peer_ip) - 1);
        cc->peer_ip[sizeof(cc->peer_ip) - 1] = '\0';
        cc->peer_port = ntohs(peer.sin_port);
        if (xTaskCreate(client_task, "hive_client", 8192, cc, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "client task spawn failed");
            free(cc);
            close(c);
        }
    }
    close(sock);
    s_ctx.listen_sock = -1;
    s_ctx.task = NULL;
    vTaskDelete(NULL);
}

/* ---- Public API ---- */

int craw_hive_ruler_start(const craw_hive_ruler_config_t *cfg) {
    if (!cfg || !cfg->hive_id || !cfg->ruler_id || !cfg->secret) return -1;
    if (s_ctx.running) return 0;
    s_ctx.cfg = *cfg;
    s_ctx.running = true;
    s_ctx.listen_sock = -1;
    memset(s_sessions, 0, sizeof(s_sessions));

    /* mDNS advertisement. Hostname must be set BEFORE service_add, or
     * service_add silently fails and the subsequent txt_set returns
     * "Invalid state or arguments." */
    mdns_init();

    /* Derive a DNS-safe lowercase hostname from the ruler_id (which is
     * typically MixedCase like "MagNET-ruler-b7a4"). DNS is case-insensitive
     * but some responders normalize and some don't — lowercase is safest. */
    char mdns_host[40];
    snprintf(mdns_host, sizeof(mdns_host), "%s", cfg->ruler_id);
    for (char *p = mdns_host; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    }
    esp_err_t merr = mdns_hostname_set(mdns_host);
    if (merr != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set('%s') failed: 0x%x", mdns_host, merr);
    }
    mdns_instance_name_set(cfg->ruler_id);

    merr = mdns_service_add(NULL, CRAW_HIVE_SERVICE_TYPE, CRAW_HIVE_SERVICE_PROTO,
                            cfg->port ? cfg->port : CRAW_HIVE_DEFAULT_PORT, NULL, 0);
    if (merr != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add failed: 0x%x", merr);
    }

    char ver[4]; snprintf(ver, sizeof(ver), "%d", CRAW_HIVE_PROTO_VERSION);
    mdns_txt_item_t txt[] = {
        {"ver",  ver},
        {"hive", (char *)cfg->hive_id},
    };
    merr = mdns_service_txt_set(CRAW_HIVE_SERVICE_TYPE, CRAW_HIVE_SERVICE_PROTO,
                                txt, sizeof(txt)/sizeof(txt[0]));
    if (merr != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_txt_set failed: 0x%x", merr);
    } else {
        ESP_LOGI(TAG, "mDNS: %s.local advertising %s.%s:%u ver=%d hive=%s",
                 mdns_host, CRAW_HIVE_SERVICE_TYPE, CRAW_HIVE_SERVICE_PROTO,
                 (unsigned)(cfg->port ? cfg->port : CRAW_HIVE_DEFAULT_PORT),
                 CRAW_HIVE_PROTO_VERSION, cfg->hive_id);
    }

    /* 10 KB stack: cJSON parse + recursive canonicalize_object + mbedTLS
     * HMAC together can push past 6 KB on Xtensa with default frame sizes,
     * and a stack overflow inside the listener task is silent. */
    if (xTaskCreate(listener_task, "craw_hive_ruler", 10240, NULL, 5, &s_ctx.task) != pdPASS) {
        s_ctx.running = false;
        return -1;
    }
    return 0;
}

void craw_hive_ruler_stop(void) {
    s_ctx.running = false;
    if (s_ctx.listen_sock >= 0) {
        shutdown(s_ctx.listen_sock, SHUT_RDWR);
        close(s_ctx.listen_sock);
    }
    mdns_service_remove(CRAW_HIVE_SERVICE_TYPE, CRAW_HIVE_SERVICE_PROTO);
}
