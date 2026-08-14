# TweetNaCl moved to the shared magnet_crypto component (punch-list H3) so
# the hive role-bundle path and this OTA path verify through one
# implementation.
idf_component_register(
    SRCS "magnet_ota.c" "transport_ip.c" "ota_verify.c" "ota_apply.c"
    INCLUDE_DIRS "include"
    REQUIRES magnet_cfg forth esp_http_client json esp_system mbedtls magnet_crypto
)
