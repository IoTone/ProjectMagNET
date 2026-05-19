idf_component_register(
    SRCS "main.c" "http_strip.c"
    INCLUDE_DIRS "."
    REQUIRES forth driver esp_hw_support led_strip
             craw_nvs craw_wifi craw_ble_provision
             esp_http_server mdns json esp_timer
             esp_wifi esp_netif nvs_flash esp_event bt
)
