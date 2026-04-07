idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver
             craw_serial craw_speaker craw_wifi craw_nvs craw_mqtt craw_http
             esp_wifi esp_netif esp_http_server nvs_flash esp_event mqtt
)
