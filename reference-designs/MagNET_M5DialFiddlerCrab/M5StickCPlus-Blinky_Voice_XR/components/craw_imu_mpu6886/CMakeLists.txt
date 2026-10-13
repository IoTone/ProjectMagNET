idf_component_register(
    SRCS "craw_imu_mpu6886.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_timer nvs_flash m5gfx
)
