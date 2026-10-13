#include <string.h>
#include "magnet_cfg.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "magnet_cfg";
static const char *NS = "magnet";

esp_err_t cfg_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* A partition from an older layout, or a full one. Erasing loses the
         * device's provisioning, so say so loudly rather than silently
         * re-provisioning-by-amnesia. */
        ESP_LOGW(TAG, "NVS unusable (%s) — erasing; device will need re-provisioning",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

bool cfg_get(const char *key, char *out, size_t out_len) {
    if (out && out_len) out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK && out[0] != '\0';
}

esp_err_t cfg_set(const char *key, const char *value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t cfg_erase(const char *key) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool cfg_provisioned(void) {
    char buf[CFG_MAX];
    return cfg_get(CFG_WIFI_SSID, buf, sizeof buf)
        && cfg_get(CFG_SERVER_URL, buf, sizeof buf)
        && cfg_get(CFG_DEV_TOKEN, buf, sizeof buf);
}
