# Host build of the NN20Clock components and their tests.
#
#   tools/test.sh                       # the whole loop
#
# or by hand:
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# This build exists so the components can be tested in seconds without a
# board. The ESP32-P4 firmware lives in firmware/nn20clock and is driven
# by tools/idf.sh inside the devcontainer; it reuses these same component
# directories unchanged as IDF components. Each component's CMakeLists.txt
# handles both cases - see the ESP_PLATFORM branch in any of them.
#
# Hardware-bound components are firmware-only and are not added here:
# nn20clock_display (ST7703 + LVGL), nn20clock_time_ui (draws with LVGL),
# and nn20clock_net (Wi-Fi lives on the ESP32-C6 co-processor). Each has
# a CMakeLists.txt that returns early when ESP_PLATFORM is not set, so
# they simply do not exist in this build.

cmake_minimum_required(VERSION 3.16)
project(nn20clock C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(NN20CLOCK_BUILD_TESTS "Build the host unit tests" ON)

if(NN20CLOCK_BUILD_TESTS)
    enable_testing()
endif()

# Bottom-up: a component only ever depends on ones listed above it.
add_subdirectory(components/nn20clock_platform)
add_subdirectory(components/nn20clock_worker)
add_subdirectory(components/nn20clock_reqpool)
add_subdirectory(components/nn20clock_workers)
add_subdirectory(components/nn20clock_time)
add_subdirectory(components/nn20clock_media)
add_subdirectory(components/nn20clock_alarm)
add_subdirectory(components/nn20clock_avi)
add_subdirectory(components/nn20clock_ringbuf)
add_subdirectory(components/nn20clock_brightness)
add_subdirectory(components/nn20clock_timer)
add_subdirectory(components/nn20clock_timefmt)
add_subdirectory(components/nn20clock_storage)
add_subdirectory(components/nn20clock_ui)
# Only its version comparison builds here; the rest of the component is
# TLS, flash writes and a reboot, and stays on the target.
add_subdirectory(components/nn20clock_ota)
add_subdirectory(components/nn20clock_manager)
add_subdirectory(components/nn20clock_app)

if(NN20CLOCK_BUILD_TESTS)
    add_subdirectory(test)
endif()
