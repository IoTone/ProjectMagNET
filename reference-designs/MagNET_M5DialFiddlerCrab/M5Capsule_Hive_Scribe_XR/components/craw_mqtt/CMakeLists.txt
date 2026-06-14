idf_component_register(
    SRCS "craw_mqtt.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt esp_event
)
