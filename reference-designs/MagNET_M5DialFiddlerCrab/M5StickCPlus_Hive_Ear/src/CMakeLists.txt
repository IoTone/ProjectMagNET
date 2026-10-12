idf_component_register(
    SRCS "main.c" "board_stickc.cpp" "ear_ble.c" "ear_audio.c" "ima_adpcm.c" "ear_atvv.c"
    INCLUDE_DIRS "."
    REQUIRES forth esp_timer driver nvs_flash bt craw_mic
)
