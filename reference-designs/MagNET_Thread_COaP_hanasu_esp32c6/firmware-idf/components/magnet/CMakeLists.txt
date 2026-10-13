idf_component_register(
    SRCS
        "magnet_core.c"
        "magnet_envelope.c"
        "magnet_pkg.c"       # OTA package verification (docs/OTA-PACKAGE.md)
        "magnet_ota_idf.c"   # OTA receiver + rollback policy (only with MN_ENABLE_OTA)
        "magnet_crypto.c"
        "magnet_link.c"
        "magnet_forth.c"
        "magnet_xfer.c"
        "magnet_bot.c"
        "magnet_led.c"
        "magnet_ot.c"
        "magnet_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES
        forth                # ESPIDFORTH engine (from EXTRA_COMPONENT_DIRS)
        craw_role_bundle     # H6: signed role bundles over HCP (BUNDLE verb)
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
        app_update         # esp_ota_* (only when MN_ENABLE_OTA=1)
        bootloader_support # esp_image_verify
        esp_partition
)
