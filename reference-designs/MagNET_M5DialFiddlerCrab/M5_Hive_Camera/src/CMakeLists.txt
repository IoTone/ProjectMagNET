idf_component_register(
    SRCS "main.c" "http_stream.c"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver
             craw_nvs craw_wifi craw_ble_provision craw_hive craw_camera
             esp_wifi esp_netif nvs_flash esp_event lwip
             esp_http_server bt
)
