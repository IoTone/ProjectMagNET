idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver esp_wifi esp_netif esp_http_server mdns nvs_flash esp_event mqtt
)
