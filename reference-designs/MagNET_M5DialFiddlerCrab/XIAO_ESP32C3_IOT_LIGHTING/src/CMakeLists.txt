idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES forth driver esp_hw_support led_strip
)
