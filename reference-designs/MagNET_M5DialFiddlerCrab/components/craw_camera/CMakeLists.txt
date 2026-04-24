idf_component_register(
    SRCS "craw_camera.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash espressif__esp32-camera
)
