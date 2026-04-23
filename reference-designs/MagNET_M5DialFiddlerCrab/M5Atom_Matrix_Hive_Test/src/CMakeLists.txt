idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver led_strip
             craw_nvs craw_wifi craw_ble_provision craw_hive
             esp_wifi esp_netif nvs_flash esp_event
             bt
)
