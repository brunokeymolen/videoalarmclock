# A sliding window over a byte stream (design 17, Milestone 11). Dual
# mode: the offset and wrapping arithmetic is pure, so it is tested on
# the host with no threads and no card. The reader that fills it and the
# player that drains it are firmware-only and live in nn20clock_player.

set(RINGBUF_SRCS
    src/nn20clock_ringbuf.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${RINGBUF_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_ringbuf STATIC ${RINGBUF_SRCS})

target_include_directories(nn20clock_ringbuf
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_ringbuf PUBLIC nn20clock_platform)

target_compile_options(nn20clock_ringbuf PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
