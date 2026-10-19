# SPDX-License-Identifier: Apache-2.0
#
# ArtemisNano_Hive_Multisense firmware. Adding the app directory to BOARD_ROOT
# makes boards/sparkfun/artemis_nano available as -b sparkfun_artemis_nano.

cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(artemis_multisense)

target_sources(app PRIVATE src/main.c)
