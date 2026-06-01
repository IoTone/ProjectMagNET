idf_component_register(
    SRCS "main.c" "aht20.c" "http_env.c"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver led_strip
             craw_nvs craw_wifi craw_ble_provision craw_hive
             esp_http_server mdns json
             esp_driver_i2c esp_driver_i2s
             esp_wifi esp_netif nvs_flash esp_event lwip
             bt
)
