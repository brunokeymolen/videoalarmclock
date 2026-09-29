# One-off ESP32-P4 app that pushes c6_cp's image to the ESP32-C6 over
# the existing SDIO link, then idles. Not the clock: put the clock back
# with tools/flash.sh afterwards. See C6-UPDATE.md at the top of the
# repository.
#
# Same configuration and partition table as the clock, so the esp_hosted
# link is set up identically and the clock's NVS (Wi-Fi, alarms) is left
# where it is.
cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../nn20clock/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(c6_updater)
