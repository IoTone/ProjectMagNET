# MagNET Hanasu — Zephyr build (XIAO MG24).
#
# The MagNET core and the Forth engine are compiled straight from
# firmware-idf/ — NOT copied — so the two targets cannot drift. What differs
# lives here: compat/ (the FreeRTOS/esp_* subset those sources call, on Zephyr
# primitives), the PSA port of magnet_crypto.c, main.c, and the Z-B bundle stub.
#
#   cd ~/zephyrproject && west build -b xiao_mg24 <this dir> -d build/mn
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(magnet_hanasu_zephyr)

set(IDF   ${CMAKE_CURRENT_LIST_DIR}/../firmware-idf)
set(MN    ${IDF}/components/magnet)
set(FORTH ${IDF}/components/forth)
set(CRAW  ${IDF}/components/craw_role_bundle)
set(NACL  ${IDF}/components/magnet_crypto)     # TweetNaCl Ed25519 (bundle signatures)

target_sources(app PRIVATE
    src/main.c
    src/magnet_crypto_psa.c
    src/ota.c
    compat/src/freertos_zephyr.c
    compat/src/nvs_zephyr.c
    compat/src/esp_misc_zephyr.c
    ${MN}/magnet_core.c
    ${MN}/magnet_envelope.c
    ${MN}/magnet_pkg.c
    ${MN}/magnet_link.c
    ${MN}/magnet_forth.c
    ${MN}/magnet_xfer.c
    ${MN}/magnet_bot.c
    ${MN}/magnet_ot.c
    ${FORTH}/forth_core.cpp
)

# compat/include first: it must win over nothing, but it is the ONLY source
# of the esp_* / freertos/* headers on this target.
target_include_directories(app PRIVATE
    compat/include
    ${MN}/include
    ${FORTH}
    ${CRAW}
)

if(CONFIG_MN_OTA_FORCE_UNHEALTHY)   # test builds only; see src/ota.c
  target_compile_definitions(app PRIVATE MN_OTA_FORCE_UNHEALTHY=1)
endif()

# BLE-GATT HCP binding: the Zephyr BT-host port when built with ble.conf,
# otherwise the IDF file's own MN_ENABLE_BLE=0 no-op stubs.
if(CONFIG_BT)
  target_sources(app PRIVATE src/magnet_ble_zephyr.c)
  # The BT stack is paid for from the malloc arena (ble.conf: 56 KB), which
  # left a BLE-fed BUNDLE COMMIT 2.5 KB from OOM with a 32 KB Forth heap.
  # Forth heap is allot/create space only (used=0 on the bench) -> 16 KB.
  target_compile_definitions(app PRIVATE MN_ENABLE_BLE=1 MN_FORTH_HEAP_KB=16)
  if(CONFIG_MN_BLE_RESIDENT)       # ble-resident.conf
    target_compile_definitions(app PRIVATE MN_BLE_RESIDENT=1)
  endif()
else()
  target_sources(app PRIVATE ${MN}/magnet_ble.c)
  target_compile_definitions(app PRIVATE MN_ENABLE_BLE=0)
endif()

target_compile_definitions(app PRIVATE
    _DEFAULT_SOURCE          # strlcpy/strcasecmp: newlib shows them by default, picolibc does not
    MN_ENABLE_OPENTHREAD=1
    MN_ENABLE_LED=0          # single PA7 LED; LED verb reports E_UNSUPPORTED
    MN_ENABLE_BOTS=0
)

# The engine takes its re-entrancy lock only under ESP_PLATFORM; this target
# is multi-threaded (link, pump, hooks), so it needs that lock too.
# MAX_WORD_LEN 64→32 saves 16 KB of dictionary BSS (512 entries); the
# longest word any MagNET/bundle code registers is 14 chars.
set_source_files_properties(${FORTH}/forth_core.cpp PROPERTIES
    COMPILE_DEFINITIONS "ESP_PLATFORM=1;MAX_WORD_LEN=32"
    COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-variable;-Wno-sign-compare;-Wno-write-strings;-Wno-missing-field-initializers")

# PBKDF2 inner loop (software SHA-256): -O3 over the project's -Os. Measured
# with the MN_KAT_TIME_PBKDF2 power-on KAT on the XIAO MG24 @ 78 MHz.
set_source_files_properties(src/magnet_crypto_psa.c PROPERTIES COMPILE_OPTIONS "-O3"
    COMPILE_DEFINITIONS MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS)   # mbedtls_md_hmac KAT

# ---- signed role bundles (H6): craw_role_bundle + TweetNaCl + cJSON ----
# Its own library because the Ed25519 component ships a header ALSO named
# magnet_crypto.h (the core's is the channel/AEAD one). IDF keeps the two
# apart per component; here each target sees only its own.
zephyr_library_named(mn_bundle)
zephyr_library_sources(
    ${CRAW}/craw_role_bundle.c
    ${NACL}/magnet_crypto.c
    ${NACL}/tweetnacl.c
    ${NACL}/nacl_glue.c
    third_party/cJSON/cJSON.c         # MIT, v1.7.x from ESP-IDF 5.3.1 components/json
)
zephyr_library_include_directories(
    compat/include
    ${NACL}/include
    ${NACL}
    ${CRAW}
    ${FORTH}
    third_party/cJSON
)
zephyr_library_compile_definitions(_DEFAULT_SOURCE)
set_source_files_properties(${NACL}/tweetnacl.c PROPERTIES COMPILE_OPTIONS
    "-Wno-shadow;-Wno-sign-compare;-Wno-unused-parameter;-Wno-char-subscripts")
target_include_directories(app PRIVATE third_party/cJSON)
target_link_libraries(app PRIVATE mn_bundle)   # not auto-linked from an app-level zephyr_library
# TF-PSA-Crypto 1.0 moved mbedtls_md_hmac() behind MBEDTLS_DECLARE_PRIVATE_
# IDENTIFIERS; the bundle engine's legacy HMAC-SHA256 check still uses it.
set_source_files_properties(${CRAW}/craw_role_bundle.c PROPERTIES
    COMPILE_DEFINITIONS MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS)
