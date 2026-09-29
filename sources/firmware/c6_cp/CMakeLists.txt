# ESP32-C6 co-processor firmware: esp_hosted's coprocessor_ota example,
# pinned to the esp_hosted release the clock's host side uses. Built and
# pushed to the C6 by tools/c6-update.sh - see C6-UPDATE.md at the top
# of the repository. Never flashed to the ESP32-P4.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c6_cp)
