idf_component_register(
    # One panel.c, two controllers (Kconfig MAGNET_BOARD): ST7789T (vendored
    # Waveshare driver) for the LCD board, JD9853 (Espressif, Apache-2.0, from the
    # Waveshare touch demo) for the touch board.
    SRCS "panel_st7789.c" "font5x7.c" "rgb_led.c" "Vernon_ST7789T.c" "esp_lcd_jd9853.c"
    INCLUDE_DIRS "include" "."
    REQUIRES driver esp_lcd esp_timer
)
