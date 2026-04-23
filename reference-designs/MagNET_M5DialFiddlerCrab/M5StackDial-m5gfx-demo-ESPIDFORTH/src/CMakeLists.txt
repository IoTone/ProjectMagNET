idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver esp_hw_support spi_flash
             craw_nvs craw_wifi craw_ble_provision craw_hive
             esp_wifi esp_netif nvs_flash esp_event lwip
             bt
)
