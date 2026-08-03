idf_component_register(
    SRCS "main.c" "console.c"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver magnet_ui magnet_cfg craw_wifi esp_wifi
)
