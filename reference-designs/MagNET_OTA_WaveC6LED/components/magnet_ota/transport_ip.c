/*
 * transport_ip — the direct-IP transport, over esp_http_client.
 *
 * Plain HTTP for R0 (plan section 5.5): the dvc_ token crosses the LAN in
 * clear text, which is acceptable to close the loop and is NOT meant to be a
 * permanent property. R1 switches to HTTPS, where the marginal cost is small
 * because D3 links mbedTLS for Ed25519 anyway.
 */
#include <string.h>
#include <stdio.h>
#include "magnet_transport.h"
#include "magnet_cfg.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "transport_ip";

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} rx_t;

static esp_err_t on_event(esp_http_client_event_t *evt) {
    rx_t *rx = (rx_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !rx) return ESP_OK;
    size_t room = (rx->cap > rx->len + 1) ? (rx->cap - rx->len - 1) : 0;
    size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
    if (take < (size_t)evt->data_len) rx->overflow = true;
    if (take) {
        memcpy(rx->buf + rx->len, evt->data, take);
        rx->len += take;
        rx->buf[rx->len] = '\0';
    }
    return ESP_OK;
}

static int ip_request(magnet_transport_t *t,
                      const char *method, const char *path,
                      const char *body,
                      char *resp, size_t resp_cap, size_t *resp_len) {
    char base[CFG_MAX], token[CFG_MAX], url[CFG_MAX + 128];

    if (!cfg_get(CFG_SERVER_URL, base, sizeof base)) {
        ESP_LOGW(TAG, "server_url not set");
        return -1;
    }
    /* Trailing slash on the configured URL plus a leading slash on the path
     * yields "//api/..." which some servers route differently — normalise. */
    size_t bl = strlen(base);
    while (bl > 0 && base[bl - 1] == '/') base[--bl] = '\0';
    snprintf(url, sizeof url, "%s%s", base, path);

    rx_t rx = { .buf = resp, .cap = resp_cap, .len = 0, .overflow = false };
    if (resp && resp_cap) resp[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = (strcmp(method, "POST") == 0) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .event_handler = on_event,
        .user_data = &rx,
        .timeout_ms = 8000,
        .disable_auto_redirect = true,
        /* NO KEEP-ALIVE. This device polls once a minute — there is nothing to
         * amortise — and a lingering connection is a socket held open in a pool
         * of CONFIG_LWIP_MAX_SOCKETS (10 by default).
         *
         * Symptom when this leaks: the board keeps a valid IP, answers pings in
         * 6 ms, and TCP connects simply time out. It works perfectly from a cold
         * boot and degrades with use, which reads as a server or network fault
         * and sent me to check the server's threads and connection table twice.
         * The server was clean both times. */
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;

    if (cfg_get(CFG_DEV_TOKEN, token, sizeof token)) {
        char auth[CFG_MAX + 16];
        snprintf(auth, sizeof auth, "Bearer %s", token);
        esp_http_client_set_header(c, "Authorization", auth);
    }
    /* Content-Type ONLY when there is a body.
     *
     * Setting it unconditionally made esp_http_client emit
     * `Content-Type: application/json` together with `Content-Length: 0` on
     * GETs, and the server returns 500 to that combination — neither header
     * alone does it. A GET has no entity, so declaring its type was meaningless
     * anyway; the server is also being fixed, because a well-formed request
     * should never 500. */
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(c);
    ESP_LOGD(TAG, "%s %s heap=%u", method, path, (unsigned)esp_get_free_heap_size());
    int status = -1;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(c);
        if (status != 200) {
            /* Log the URL we ACTUALLY built, not the one we meant to. A status
             * alone tells you the server was unhappy; it does not tell you
             * whether the path, the query string or the base URL was mangled on
             * the way out, which is where these bugs live. */
            ESP_LOGW(TAG, "%s %s -> %d", method, url, status);
            if (rx.len) ESP_LOGW(TAG, "  body: %.200s", resp);
        }
        if (resp_len) *resp_len = rx.len;
        if (rx.overflow) {
            /* Say so rather than letting a silently clipped body be parsed as
             * if it were whole — truncated JSON usually parses as "no update",
             * which is the most dangerous possible misreading here. */
            ESP_LOGW(TAG, "response truncated at %u bytes", (unsigned)rx.cap);
            status = -2;
        }
    } else {
        ESP_LOGW(TAG, "%s %s failed: %s", method, url, esp_err_to_name(err));
    }
    /* Close explicitly before cleanup rather than trusting cleanup to do it on
     * every path — including the error paths, which are the ones that leak. */
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

static esp_err_t ip_open(magnet_transport_t *t)  { (void)t; return ESP_OK; }
static void      ip_close(magnet_transport_t *t) { (void)t; }

static magnet_transport_t s_ip = {
    .name = "ip",
    .open = ip_open,
    .request = ip_request,
    .close = ip_close,
    .ctx = NULL,
};

magnet_transport_t *transport_ip(void) { return &s_ip; }
