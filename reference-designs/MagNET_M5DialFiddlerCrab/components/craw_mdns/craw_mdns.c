#include "craw_mdns.h"
#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "craw_mdns";

void craw_mdns_start(const char *hostname, const char *instance_name) {
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(hostname);
    mdns_instance_name_set(instance_name);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", hostname);
}
