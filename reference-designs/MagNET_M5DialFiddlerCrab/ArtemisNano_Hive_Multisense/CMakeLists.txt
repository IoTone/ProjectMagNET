# SPDX-License-Identifier: Apache-2.0
#
# ArtemisNano_Hive_Multisense firmware. Adding the app directory to BOARD_ROOT
# makes boards/sparkfun/artemis_nano available as -b sparkfun_artemis_nano.

cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(artemis_multisense)

target_sources(app PRIVATE src/main.c src/rv1805.c src/opt4048.c src/bno085.c
  src/hive_ble.c)

# CEVA SH-2 sensor hub library (vendored, unmodified; see lib/sh2/VENDORED.txt).
target_sources(app PRIVATE
  lib/sh2/sh2.c
  lib/sh2/sh2_SensorValue.c
  lib/sh2/sh2_util.c
  lib/sh2/shtp.c
)
target_include_directories(app PRIVATE lib/sh2)
