idf_component_register(
    SRCS "main.c" "board_stickc.cpp" "presence.c" "stats.c" "activity.c" "portal.c" "net_portal.c"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver nvs_flash esp_netif lwip esp_wifi esp_http_client esp_http_server mbedtls
             craw_serial craw_wifi craw_nvs craw_mqtt
)
