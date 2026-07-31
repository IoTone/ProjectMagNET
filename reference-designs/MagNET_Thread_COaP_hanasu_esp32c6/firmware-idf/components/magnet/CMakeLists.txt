idf_component_register(
    SRCS
        "magnet_core.c"
        "magnet_envelope.c"
        "magnet_link.c"
        "magnet_forth.c"
        "magnet_ot.c"
    INCLUDE_DIRS "include"
    REQUIRES
        forth                # ESPIDFORTH engine (from EXTRA_COMPONENT_DIRS)
        openthread           # esp_openthread_* (only used when MN_ENABLE_OPENTHREAD=1)
        esp_netif
        esp_event
        nvs_flash
        esp_hw_support     # esp_random, esp_chip_info
        spi_flash          # esp_flash_get_size (SYSINFO)
        vfs                  # esp_vfs_eventfd
        esp_timer
        driver
)
