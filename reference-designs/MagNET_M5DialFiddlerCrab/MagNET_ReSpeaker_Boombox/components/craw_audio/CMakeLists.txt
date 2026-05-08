idf_component_register(
    SRCS "craw_audio_i2s.c"
         "craw_audio_synth.c"
         "craw_audio_patterns.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer
)
