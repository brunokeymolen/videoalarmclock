# Formatting a time into the text a screen draws. Dual-mode: the screen
# needs LVGL and is firmware-only, but this does not and is tested on the
# host in seconds.

set(TIMEFMT_SRCS
    src/nn20clock_timefmt.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${TIMEFMT_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform nn20clock_timer
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_timefmt STATIC ${TIMEFMT_SRCS})

target_include_directories(nn20clock_timefmt
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_timefmt
    PUBLIC nn20clock_platform nn20clock_timer
)

target_compile_options(nn20clock_timefmt PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
