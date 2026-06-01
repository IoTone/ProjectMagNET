idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver led_strip
             craw_serial craw_wifi craw_nvs
             esp_wifi esp_netif esp_http_server nvs_flash esp_event
             bt
)
