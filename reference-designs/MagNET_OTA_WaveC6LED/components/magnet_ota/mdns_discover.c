/*
 * mdns_discover — R2: resolve `_robotarme._tcp` instead of reading NVS.
 *
 * mDNS is DISCOVERY, not transport (plan §5.1): this answers "what is the
 * base URL", and the bytes still move over transport_ip's HTTP path. The
 * server side (S2, robotarme 0.70.0) answers a PTR question with SRV+TXT+A
 * bundled, so one query resolves everything in a single round trip.
 *
 * Precedence is deliberate: an NVS `server_url` ALWAYS WINS. Discovery only
 * fills the gap when nothing is seeded — so a bench device with a pinned URL
 * keeps it, and a fresh-from-the-bag device on the right LAN needs no
 * provisioning step at all. The discovered URL is cached until reboot or the
 * next explicit `discover`; a flapping advertisement must not move a device
 * between servers mid-session.
 */
#include <string.h>
#include <stdio.h>
#include "magnet_ota.h"
#include "magnet_cfg.h"
#include "craw_wifi.h"
#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "mdns_discover";

static char s_url[CFG_MAX];
static bool s_have;

const char *ota_discovered_url(void) { return s_have ? s_url : NULL; }

bool ota_discover_server(uint32_t timeout_ms) {
    if (!craw_wifi_is_connected()) {
        ESP_LOGW(TAG, "not on wifi — nothing to query");
        return false;
    }
    static bool inited = false;
    if (!inited) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
            return false;
        }
        inited = true;
    }
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_robotarme", "_tcp", timeout_ms, 4, &results);
    if (err != ESP_OK || !results) {
        ESP_LOGW(TAG, "no _robotarme._tcp answer within %lu ms",
                 (unsigned long)timeout_ms);
        return false;
    }
    bool found = false;
    for (mdns_result_t *r = results; r && !found; r = r->next) {
        uint16_t port = r->port ? r->port : 80;
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                /* R0 is plain HTTP (plan §5.5); R1 flips this scheme. */
                snprintf(s_url, sizeof s_url, "http://" IPSTR ":%u",
                         IP2STR(&a->addr.u_addr.ip4), (unsigned)port);
                s_have = true;
                found = true;
                ESP_LOGI(TAG, "discovered %s (%s)", s_url,
                         r->instance_name ? r->instance_name : "?");
                break;
            }
        }
    }
    mdns_query_results_free(results);
    return found;
}
