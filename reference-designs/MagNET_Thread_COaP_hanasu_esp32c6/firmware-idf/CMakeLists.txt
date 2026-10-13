# MagNET Hanasu — ESP-IDF firmware (E-Phase A spike)
#
# NOTE: do NOT set(EXTRA_COMPONENT_DIRS ...) here — PlatformIO uses that variable
# to map src/ to the special `main` component, and overwriting it makes the build
# fail with "Couldn't find the main target of the project!". The ESPIDFORTH
# `forth` component is instead pulled in as an in-tree symlink at components/forth
# (created with: ln -s ../../../MagNET_M5DialFiddlerCrab/ESPIDFORTH/components/forth
#  components/forth), exactly mirroring ESPIDFORTH's own working layout.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(magnet_hanasu)
