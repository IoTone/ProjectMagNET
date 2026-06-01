idf_component_register(
    SRCS "craw_redis_server.c"
         "craw_redis_storage_nvs.c"
         "craw_redis_client.c"
    INCLUDE_DIRS "."
    REQUIRES esp_netif esp_event lwip nvs_flash esp_timer
)
