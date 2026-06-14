idf_component_register(
    SRCS "craw_role_bundle.c"
    INCLUDE_DIRS "."
    REQUIRES forth nvs_flash mbedtls json esp_rom
)
