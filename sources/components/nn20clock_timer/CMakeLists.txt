# The Timer (design 6, 7). Dual-mode: an IDF component on the target, a
# static library on the host so test/ can exercise it under ctest.

set(TIMER_SRCS
    src/nn20clock_timer.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${TIMER_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform nn20clock_time nn20clock_alarm
        # esp_timer drives the tick on the target; on the host there is
        # no automatic tick and tests call nn20clock_timer_poll().
        PRIV_REQUIRES nn20clock_worker esp_timer
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_timer STATIC ${TIMER_SRCS})

target_include_directories(nn20clock_timer
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_timer
    PUBLIC  nn20clock_platform nn20clock_time nn20clock_alarm
    PRIVATE nn20clock_worker
)

target_compile_options(nn20clock_timer PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
