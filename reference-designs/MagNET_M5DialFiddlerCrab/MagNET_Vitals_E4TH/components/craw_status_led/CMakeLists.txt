idf_component_register(
    SRCS "craw_status_led.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_rmt
    PRIV_REQUIRES led_strip
)
