cmake_minimum_required(VERSION 3.16)

# Reuse the four target-agnostic components from the sibling M5Stamp E4TH
# project (forth, craw_nvs, craw_wifi, craw_ble_provision). Listing each
# path individually keeps IDF from also discovering `craw_hive`,
# `craw_serial`, and the managed `mdns`/`led_strip` mirrors that would
# require additional registry components we don't pull in.
#
# `led_strip` is *vendored* into our own components/ directory (see
# components/led_strip/, copied verbatim from the espressif/led_strip
# registry component v3.0.3). Self-contained: a fresh checkout builds
# without first having to build M5Stamp.
set(M5STAMP_COMPONENTS
    "${CMAKE_CURRENT_LIST_DIR}/../M5Stamp3CU_Blinky_E4TH/components")
# APPEND, don't `set()` — PlatformIO passes -DEXTRA_COMPONENT_DIRS=<src_dir>
# on the command line so `src/` becomes the main component. A bare `set()`
# wipes that out, leaving the project with no main, an empty __pio_env, and
# a `undefined reference to app_main` at link time.
list(APPEND EXTRA_COMPONENT_DIRS
    "${M5STAMP_COMPONENTS}/forth"
    "${M5STAMP_COMPONENTS}/craw_nvs"
    "${M5STAMP_COMPONENTS}/craw_wifi"
    "${M5STAMP_COMPONENTS}/craw_ble_provision"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(magnet_vitals_e4th)
