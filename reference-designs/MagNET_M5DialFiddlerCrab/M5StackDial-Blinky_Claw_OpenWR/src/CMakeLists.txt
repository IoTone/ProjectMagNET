idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver
             claw_serial claw_speaker claw_wifi claw_nvs claw_mqtt claw_http
             esp_wifi esp_netif esp_http_server nvs_flash esp_event mqtt
)
