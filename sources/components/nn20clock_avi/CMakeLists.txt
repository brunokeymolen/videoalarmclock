# Reading an AVI file (design 16). Dual-mode: the container format is
# pure byte-wrangling over a reader callback, so it is tested on the
# host against buffers built in the test - no card, no filesystem. The
# hardware-bound half of playback (JPEG decode, I2S) lives elsewhere.

set(AVI_SRCS
    src/nn20clock_avi.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${AVI_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_avi STATIC ${AVI_SRCS})

target_include_directories(nn20clock_avi
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_avi PUBLIC nn20clock_platform)

target_compile_options(nn20clock_avi PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
