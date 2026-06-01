idf_component_register(
    SRCS "craw_http.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server craw_mqtt
)
