idf_component_register(
    SRCS "main.c" "http_vitals.c"
    INCLUDE_DIRS "."
    REQUIRES craw_mr60bha2 craw_status_led craw_bh1750
             forth craw_nvs craw_wifi craw_ble_provision
             driver esp_timer esp_http_server mdns
             esp_wifi esp_netif nvs_flash esp_event
             bt
)
