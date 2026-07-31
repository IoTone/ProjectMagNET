idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES magnet forth driver esp_timer nvs_flash
)
