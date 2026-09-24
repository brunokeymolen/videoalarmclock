# The alarm model and its occurrence arithmetic (design 10). Dual-mode:
# an IDF component on the target, a static library on the host.
#
# Deliberately pure - no state, no threads, no storage. The firing rules
# are the part of this clock most worth testing exhaustively, and this
# is what lets that happen on a desktop instead of at 7am.

set(ALARM_SRCS
    src/nn20clock_alarm.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${ALARM_SRCS}
        INCLUDE_DIRS "include"
        REQUIRES nn20clock_platform nn20clock_time nn20clock_media
    )
    return()
endif()

# ----------------------------------------------------------- host build --

add_library(nn20clock_alarm STATIC ${ALARM_SRCS})

target_include_directories(nn20clock_alarm
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nn20clock_alarm
    PUBLIC nn20clock_platform nn20clock_time nn20clock_media
)

target_compile_options(nn20clock_alarm PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
)
