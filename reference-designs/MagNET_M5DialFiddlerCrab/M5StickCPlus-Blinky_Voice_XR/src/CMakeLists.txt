idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver esp_driver_i2s
             craw_serial craw_speaker craw_wifi craw_nvs craw_mqtt craw_http
             craw_mic craw_imu_mpu6886
             esp_wifi esp_netif esp_http_server nvs_flash esp_event mqtt
)
