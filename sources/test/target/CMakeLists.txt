# On-target test runner. Same suites as the host build, flashed to the
# board. Driven by tools/idf-test.sh, and run sparingly - tools/test.sh
# is the everyday loop.
#
#   NN20CLOCK_IDF_PROJECT=test/target tools/idf.sh set-target esp32p4

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components"
)

# Inherit the firmware's board configuration, then override only what a
# test app needs differently. Sharing the file rather than copying it is
# the point: an on-target run that used default PSRAM, flash, or cache
# settings would be testing a different machine than the firmware runs
# on, which is most of the reason to run on target at all.
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../../firmware/nn20clock/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nn20clock_target_test)
