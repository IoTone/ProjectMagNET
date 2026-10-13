# TweetNaCl moved to the shared magnet_crypto component (punch-list H3) so
# the hive role-bundle path and this OTA path verify through one
# implementation.
idf_component_register(
    SRCS "magnet_ota.c" "transport_ip.c" "ota_verify.c" "ota_apply.c" "mdns_discover.c"
         # R3: the relay transport (shared by BLE and serial) and its BLE pipe.
         "relay_frame.c" "transport_relay.c" "pipe_ble.c" "pipe_serial.c"
    INCLUDE_DIRS "include"
    REQUIRES magnet_cfg forth esp_http_client json esp_system mbedtls magnet_crypto craw_wifi bt
)

# R1: pin a CA by dropping server_ca.pem beside this file. Embedded into the
# image when present (the only channel a PEM fits through — CFG_MAX is 128);
# absent, transport_ip falls back to the IDF public-CA bundle.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server_ca.pem")
    target_add_binary_data(${COMPONENT_LIB} "server_ca.pem" TEXT)
    # __has_include probe target: generate the marker header the source checks
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/include/server_ca_pem.h"
         "/* generated: server_ca.pem is embedded */\n")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/include")
endif()
