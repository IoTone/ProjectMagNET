idf_component_register(
    SRCS "claw_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event
)
