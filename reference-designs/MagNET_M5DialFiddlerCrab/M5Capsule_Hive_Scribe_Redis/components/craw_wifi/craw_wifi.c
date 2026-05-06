#include "craw_wifi.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "craw_wifi";

#define WIFI_MAX_RETRY 5

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
    while (wifi_retry_count < WIFI_MAX_RETRY && !wifi_connected) {
        wifi_retry_count++;
        ESP_LOGW(TAG, "Retry %d/%d...", wifi_retry_count, WIFI_MAX_RETRY);
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!wifi_connected) {
            esp_wifi_connect();
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    if (!wifi_connected) {
        ESP_LOGE(TAG, "Connect failed after %d retries", WIFI_MAX_RETRY);
        fire_event(CRAW_WIFI_EVENT_CONNECT_FAILED);
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

    wifi_retry_count = 0;

    if (wifi_started) {
        // Already started: disconnect, apply new config, reconnect.
        reconnect_pending = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));  // Let disconnect settle
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "connect error: %d", err);
        }
        reconnect_pending = false;
    } else {
        // First-time start
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    }
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
