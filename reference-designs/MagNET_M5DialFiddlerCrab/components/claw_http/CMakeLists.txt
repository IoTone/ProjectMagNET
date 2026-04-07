idf_component_register(
    SRCS "claw_http.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server claw_mqtt
)
