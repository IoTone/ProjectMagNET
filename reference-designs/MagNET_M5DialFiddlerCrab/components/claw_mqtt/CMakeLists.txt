idf_component_register(
    SRCS "claw_mqtt.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt esp_event
)
