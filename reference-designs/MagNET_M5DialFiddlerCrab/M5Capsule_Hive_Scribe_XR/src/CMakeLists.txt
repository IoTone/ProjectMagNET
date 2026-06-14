idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver
             craw_nvs craw_wifi craw_ble_provision craw_hive craw_role_bundle craw_mqtt
             craw_redis craw_imu
             esp_wifi esp_netif nvs_flash esp_event lwip
             bt mqtt
)
