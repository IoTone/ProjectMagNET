idf_component_register(
    SRCS "main.c" "console.c"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver magnet_ui magnet_cfg magnet_ota craw_wifi esp_wifi
)
