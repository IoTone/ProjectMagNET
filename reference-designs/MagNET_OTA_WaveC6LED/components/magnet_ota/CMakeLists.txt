idf_component_register(
    SRCS "magnet_ota.c" "transport_ip.c"
    INCLUDE_DIRS "include"
    REQUIRES magnet_cfg forth esp_http_client json
)
