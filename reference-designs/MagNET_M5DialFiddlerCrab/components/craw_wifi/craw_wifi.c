#include "craw_wifi.h"

#include <stdlib.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "craw_wifi";

#define WIFI_MAX_RETRY 15
/* After a full WIFI_MAX_RETRY burst, sleep this long then start a new
 * burst. Mesh / ISP APs sometimes lock out a client for tens of seconds
 * after repeated failures; this gives the AP a chance to recover. Set
 * to 0 to disable respawn (= old "give up forever" behaviour). */
#define WIFI_RESPAWN_DELAY_MS 30000

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static volatile bool     wifi_connected     = false;
static int               wifi_retry_count   = 0;
static bool              wifi_started       = false;
static volatile bool     reconnect_pending  = false;
static esp_netif_t      *sta_netif          = NULL;

static craw_wifi_event_cb_t s_event_cb  = NULL;
static void                *s_event_ctx = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void fire_event(craw_wifi_event_t ev)
{
    if (s_event_cb) {
        s_event_cb(ev, s_event_ctx);
    }
}

// ---------------------------------------------------------------------------
// Retry task — spawned on unexpected disconnect
// ---------------------------------------------------------------------------
static void wifi_retry_task(void *arg)
{
    for (;;) {
        while (wifi_retry_count < WIFI_MAX_RETRY && !wifi_connected) {
            wifi_retry_count++;
            ESP_LOGW(TAG, "Retry %d/%d...", wifi_retry_count, WIFI_MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (!wifi_connected) {
                esp_wifi_connect();
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        if (wifi_connected) break;

        ESP_LOGE(TAG, "Connect failed after %d retries", WIFI_MAX_RETRY);
        fire_event(CRAW_WIFI_EVENT_CONNECT_FAILED);

        if (WIFI_RESPAWN_DELAY_MS <= 0) break;   /* give up forever */

        /* Mesh / ISP APs sometimes blacklist a client for tens of seconds
         * after repeated failures. Wait, then start a fresh burst so the
         * device eventually recovers without needing a reboot. */
        ESP_LOGW(TAG, "Sleeping %d ms before next retry burst...",
                 WIFI_RESPAWN_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(WIFI_RESPAWN_DELAY_MS));
        if (wifi_connected) break;   /* something else connected us */
        wifi_retry_count = 0;        /* arm next burst */
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// ESP event handler (WIFI_EVENT + IP_EVENT)
// ---------------------------------------------------------------------------
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disc =
                (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Disconnected, reason=%d", disc->reason);
            wifi_connected = false;
            fire_event(CRAW_WIFI_EVENT_DISCONNECTED);

            // Skip retry task if this is an intentional reconnect (new creds)
            if (!reconnect_pending && wifi_retry_count == 0) {
                xTaskCreate(wifi_retry_task, "wifi_retry", 3072, NULL, 3, NULL);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        wifi_retry_count = 0;
        fire_event(CRAW_WIFI_EVENT_CONNECTED);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void craw_wifi_init(const char *hostname, craw_wifi_event_cb_t cb, void *cb_ctx)
{
    s_event_cb  = cb;
    s_event_ctx = cb_ctx;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    if (hostname && hostname[0]) {
        esp_netif_set_hostname(sta_netif, hostname);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}

/* Shared apply-config-and-(re)start helper. Both craw_wifi_connect and
 * craw_wifi_connect_bssid build a wifi_config_t and hand it here. */
static void apply_and_connect(const wifi_config_t *wifi_config)
{
    wifi_retry_count = 0;

    if (wifi_started) {
        // Already started: disconnect, apply new config, reconnect.
        reconnect_pending = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));  // Let disconnect settle
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,
                                            (wifi_config_t *)wifi_config));
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "connect error: %d", err);
        }
        reconnect_pending = false;
    } else {
        // First-time start
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,
                                            (wifi_config_t *)wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    }
}

void craw_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "No SSID provided, skipping connect");
        return;
    }

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid,
            sizeof(wifi_config.sta.ssid) - 1);

    if (pass && pass[0] != '\0') {
        strncpy((char *)wifi_config.sta.password, pass,
                sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        wifi_config.sta.password[0] = '\0';
    }
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    apply_and_connect(&wifi_config);
}

void craw_wifi_connect_bssid(const char *ssid, const char *pass,
                             const uint8_t bssid[6])
{
    if (!ssid || ssid[0] == '\0' || !bssid) {
        ESP_LOGW(TAG, "connect_bssid: missing ssid or bssid, skipping");
        return;
    }

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid,
            sizeof(wifi_config.sta.ssid) - 1);

    if (pass && pass[0] != '\0') {
        strncpy((char *)wifi_config.sta.password, pass,
                sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        wifi_config.sta.password[0] = '\0';
    }

    /* BSSID pinning: bypass band-steering / mesh roaming during the
     * initial association. The driver will only attempt to associate
     * with this exact AP MAC; AP-side hints to switch nodes/bands are
     * ignored until association completes. */
    wifi_config.sta.bssid_set = true;
    memcpy(wifi_config.sta.bssid, bssid, 6);

    /* Channel scan + signal sort are irrelevant once bssid_set is true,
     * but leaving them set doesn't hurt — they apply on subsequent
     * driver-internal reconnect attempts if any. */
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    /* Disable PMF on the client side. Stops the C3 from advertising
     * WPA3 capability flags, which avoids transition-mode negotiation
     * failures on WPA2/WPA3-mixed APs (a common ISP-router default). */
    wifi_config.sta.pmf_cfg.capable  = false;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG, "Connecting to '%s' pinned to "
        "%02x:%02x:%02x:%02x:%02x:%02x (PMF off)",
        ssid, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);

    apply_and_connect(&wifi_config);
}

void craw_wifi_connect_best(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGW(TAG, "connect_best: no SSID, skipping");
        return;
    }

    /* Scan first, find the strongest BSSID matching ssid. */
    craw_wifi_scan_result_t results[20];
    int n = craw_wifi_scan(results, 20);
    if (n <= 0) {
        ESP_LOGW(TAG, "connect_best: scan returned %d, falling back to plain connect", n);
        craw_wifi_connect(ssid, pass);
        return;
    }

    /* Results are sorted by RSSI desc, so the first match is the
     * strongest. */
    for (int i = 0; i < n; i++) {
        if (strcmp(results[i].ssid, ssid) == 0) {
            ESP_LOGI(TAG, "connect_best: '%s' best BSSID on ch %u @ %d dBm",
                ssid, (unsigned)results[i].channel, (int)results[i].rssi);
            craw_wifi_connect_bssid(ssid, pass, results[i].bssid);
            return;
        }
    }

    ESP_LOGW(TAG, "connect_best: '%s' not seen in scan (n=%d), falling back to plain connect",
        ssid, n);
    craw_wifi_connect(ssid, pass);
}

void craw_wifi_disconnect(void)
{
    if (wifi_started) {
        reconnect_pending = true;
        esp_wifi_disconnect();
        reconnect_pending = false;
    }
}

bool craw_wifi_is_connected(void)
{
    return wifi_connected;
}

bool craw_wifi_get_ip_str(char *buf, int len)
{
    if (!wifi_connected || !sta_netif || !buf || len < 16) {
        return false;
    }
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(sta_netif, &ip_info) != ESP_OK) {
        return false;
    }
    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

void *craw_wifi_get_netif(void)
{
    return (void *)sta_netif;
}

// ---------------------------------------------------------------------------
// WiFi scan
// ---------------------------------------------------------------------------

/* qsort comparator — RSSI descending (strongest first). */
static int rssi_desc_cmp(const void *a, const void *b)
{
    const craw_wifi_scan_result_t *A = (const craw_wifi_scan_result_t *)a;
    const craw_wifi_scan_result_t *B = (const craw_wifi_scan_result_t *)b;
    /* int8 RSSI is negative; -40 > -80, so plain subtraction works. */
    return (int)B->rssi - (int)A->rssi;
}

int craw_wifi_scan(craw_wifi_scan_result_t *results, int max_results)
{
    if (!results || max_results <= 0) return -1;

    /* esp_wifi_scan_start requires the driver to be started + in STA mode.
     * craw_wifi_init sets WIFI_MODE_STA, but if nothing has ever called
     * connect(), esp_wifi_start() hasn't fired yet. Bring it up now. */
    if (!wifi_started) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGE(TAG, "scan: esp_wifi_start failed: %s", esp_err_to_name(err));
            return -1;
        }
        wifi_started = true;
    }

    /* If an attempt is in flight (esp_wifi_connect dispatched, retry task
     * mid-loop, etc.) esp_wifi_scan_start returns ESP_ERR_WIFI_STATE with
     * the driver logging "STA is connecting, scan are not allowed!" — abort
     * by disconnecting first. The brief delay lets the WIFI_EVENT_STA_
     * DISCONNECTED event propagate so the driver state machine is settled
     * by the time we start the scan. Setting `reconnect_pending = true`
     * during the disconnect prevents wifi_event_handler from spawning a
     * new retry task that would race the scan. */
    reconnect_pending = true;
    esp_wifi_disconnect();   /* OK if not currently connecting */
    vTaskDelay(pdMS_TO_TICKS(250));
    reconnect_pending = false;

    wifi_scan_config_t cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,                    /* 0 = all channels */
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = { .min = 100, .max = 200 },  /* ms per channel */
        },
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, true /* blocking */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) return 0;

    /* Cap to the caller's buffer. The driver internally retains up to
     * CONFIG_WIFI_SCAN_AP_LIST_LEN records; the remainder is discarded. */
    uint16_t fetch_count = ap_count;
    if (fetch_count > max_results) fetch_count = max_results;

    wifi_ap_record_t *records = malloc((size_t)fetch_count * sizeof(wifi_ap_record_t));
    if (!records) {
        ESP_LOGE(TAG, "scan: out of memory (need %u records)", fetch_count);
        return -1;
    }

    err = esp_wifi_scan_get_ap_records(&fetch_count, records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: esp_wifi_scan_get_ap_records failed: %s",
            esp_err_to_name(err));
        free(records);
        return -1;
    }

    /* Convert wifi_ap_record_t to our public struct. */
    for (uint16_t i = 0; i < fetch_count; i++) {
        strncpy(results[i].ssid, (const char *)records[i].ssid,
                sizeof(results[i].ssid) - 1);
        results[i].ssid[sizeof(results[i].ssid) - 1] = '\0';
        results[i].rssi     = records[i].rssi;
        results[i].channel  = records[i].primary;
        results[i].authmode = (uint8_t)records[i].authmode;
        memcpy(results[i].bssid, records[i].bssid, 6);
    }

    free(records);

    /* Sort strongest-first so the human reading the table sees the most
     * likely connect candidates at the top. The driver returns records in
     * channel-scan order, not RSSI order. */
    qsort(results, fetch_count, sizeof(craw_wifi_scan_result_t), rssi_desc_cmp);

    return (int)fetch_count;
}
