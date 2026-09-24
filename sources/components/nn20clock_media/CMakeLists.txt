# What counts as media and how it is named (design 13). Dual-mode: the
# SD card and the FTP server are firmware-only, but the rules they both
# enforce - which extensions are media, which names are safe to serve -
# are pure logic and tested on the host.

set(MEDIA_SRCS
    src/nn20clock_media.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MEDIA_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_media STATIC ${MEDIA_SRCS})

target_include_directories(nn20clock_media
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_media PUBLIC nn20clock_platform)

target_compile_options(nn20clock_media PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
