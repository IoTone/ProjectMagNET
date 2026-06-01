idf_component_register(
    SRCS "craw_imu.c"
         "craw_imu_http.c"
         # Vendored Bosch BMI270 driver from espressif/bmi270 1.0.1~1
         # (Apache-2.0). Pulled into the component so we don't drag
         # the espressif/sensor_hub + espressif/i2c_bus transitives,
         # the latter of which calls i2c_master_get_bus_handle (ESP-IDF
         # v5.4 only). Our platform is espressif32@6.9.0 → IDF v5.3.1.
         # The bmi270 driver itself uses only v5.3-compatible APIs
         # (i2c_master_bus_add_device + i2c_master_transmit_receive)
         # so it builds clean.
         "vendor/bmi270/bmi270.c"
    INCLUDE_DIRS "."
                 "vendor/bmi270"
    REQUIRES driver esp_timer esp_http_server
)
