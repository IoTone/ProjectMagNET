# MagNET OTA client — Waveshare ESP32-C6-LCD-1.47 ("WaveC6LED")
#
# Plain ESP-IDF, built with idf.py. The ESPIDFORTH project this forks from is
# PlatformIO-first, which injects src/ as the main component; idf.py does not,
# so that project links with `undefined reference to app_main` under a bare
# `idf.py build`. Here the app lives in main/ — the layout idf.py expects — and
# PlatformIO can still be pointed at it if anyone wants both.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(magnet_ota_wavec6led)
