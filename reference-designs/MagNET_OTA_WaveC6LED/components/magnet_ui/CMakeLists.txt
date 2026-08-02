idf_component_register(
    SRCS "panel_st7789.c" "font5x7.c" "rgb_led.c" "Vernon_ST7789T.c"
    INCLUDE_DIRS "include" "."
    REQUIRES driver esp_lcd esp_timer
)
