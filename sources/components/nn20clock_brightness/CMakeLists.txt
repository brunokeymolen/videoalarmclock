# The day/night brightness schedule. Dual mode: deciding which
# brightness applies at a given minute is pure arithmetic with a
# midnight wrap in it, so it is tested on the host with no display, no
# clock and no board. Applying the answer to the backlight is
# nn20clock_display's job and the wiring is the app's.

set(BRIGHTNESS_SRCS
    src/nn20clock_brightness.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${BRIGHTNESS_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_brightness STATIC ${BRIGHTNESS_SRCS})

target_include_directories(nn20clock_brightness
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_brightness PUBLIC nn20clock_platform)

target_compile_options(nn20clock_brightness PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
