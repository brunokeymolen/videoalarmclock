# ESP32-P4 firmware for NN20Clock.
#
#   tools/idf.sh set-target esp32p4
#   tools/idf.sh build
#   tools/idf.sh -p /dev/ttyACM0 flash monitor
#
# The components in ../../components are pulled in unchanged, so the same
# sources that run under ctest on the host run on the board.

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components"
)

# One defaults file, and no local override.
#
# There was a sdkconfig.defaults.local here, git-ignored, holding a real
# SSID and password - and Kconfig strings are .rodata, so they travelled
# inside every image built from this tree, public release assets
# included. The whole mechanism was removed on 2026-09-06 rather than
# guarded: the network is chosen on the device now (gear icon -> Wi-Fi,
# stored in NVS), so nothing secret needs to reach a build at all.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nn20clock)
