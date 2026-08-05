idf_component_register(
    SRCS
        "magnet_core.c"
        "magnet_envelope.c"
        "magnet_crypto.c"
        "magnet_link.c"
        "magnet_forth.c"
        "magnet_bot.c"
        "magnet_ot.c"
        "magnet_ble.c"
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
        mbedtls            # HKDF/PBKDF2/CCM/base64 (magnet_crypto)
        bt                 # NimBLE (only when MN_ENABLE_BLE=1)
)
